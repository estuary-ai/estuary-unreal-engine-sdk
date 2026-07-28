// EstuarySubsystem.cpp — Socket.IO lifecycle + reconnect backoff (XPORT-01..07).
//
// State machine transitions (SetState is the single gate):
//   Disconnected -> Connecting    : Connect() called
//   Connecting   -> Connected     : HandleSessionInfo (session_info received)
//   Connecting   -> Error         : HandleAuthError / HandleSessionRejected
//   Connected    -> Reconnecting  : HandleTransportClosed (non-1000 code)
//   Reconnecting -> Connecting    : TickReconnect fires after backoff delay
//   *            -> Disconnected  : Disconnect() or Deinitialize()
//   *            -> Error         : HandleTransportError
//
// Exponential backoff: 2^attempt capped at 30s.
//   Attempt 0 -> 1s, 1 -> 2s, 2 -> 4s, 3 -> 8s, 4 -> 16s, 5+ -> 30s.
//
// All .Broadcast() calls are guarded structurally: every Handle* method asserts
// check(IsInGameThread()) before touching any UObject or delegate (Pitfall 17).

#include "EstuarySubsystem.h"
#include "EstuarySocketIOBridge.h"
#include "EstuaryCharacterComponent.h"
#include "EstuaryCameraComponent.h"
#include "EstuaryDeveloperSettings.h"
#include "EstuaryLog.h"
#include "EstuaryMarshal.h"
#include "EstuaryMicrophoneComponent.h"
#include "EstuaryAudioPlayerComponent.h"

#include "Containers/Ticker.h"
#include "Containers/Map.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Actor.h"
#include "Math/UnrealMathUtility.h"

// ============================================================================
// Initialize / Deinitialize
// ============================================================================

void UEstuarySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    UE_LOG(LogEstuary, Log, TEXT("[Subsystem] Initialize — state=Disconnected"));
    SessionState = EEstuarySessionState::Disconnected;
    // Bridge is NOT created here — constructed lazily in Connect() so PIE runs
    // with no connection don't create sockets prematurely (Pitfall 3 PIE hygiene).
}

void UEstuarySubsystem::Deinitialize()
{
    UE_LOG(LogEstuary, Log, TEXT("[Subsystem] Deinitialize — tearing down bridge + reconnect timer"));

    CancelReconnect();

    // Phase 8: leave LiveKit room on session teardown (PIE stop, game exit).
    if (CurrentVoiceMode == EEstuaryVoiceMode::LiveKit)
    {
        LeaveLiveKitRoom();
    }

    if (Bridge.IsValid())
    {
        // Clear sinks FIRST so any late-arriving marshaled callbacks see null
        // TFunctions and silently no-op rather than touching a dying subsystem.
        Bridge->OnSessionInfo        = nullptr;
        Bridge->OnAuthError          = nullptr;
        Bridge->OnSessionRejected    = nullptr;
        Bridge->OnError              = nullptr;
        Bridge->OnQuotaExceeded      = nullptr;
        Bridge->OnMemoryUpdated      = nullptr;
        Bridge->OnTransportConnected = nullptr;
        Bridge->OnTransportClosed    = nullptr;
        Bridge->OnTransportError     = nullptr;
        Bridge->OnBotResponse        = nullptr;
        Bridge->OnInterruptAck       = nullptr;
        Bridge->OnBotVoiceSink       = nullptr;
        Bridge->OnSttResponseSink    = nullptr;
        Bridge->OnCameraCaptureSink  = nullptr;
        Bridge->OnBotAnimationSink   = nullptr;
        Bridge->OnBotPoseSink        = nullptr;
        // Phase 8: clear LiveKit sinks.
        Bridge->OnLiveKitTokenSink   = nullptr;
        Bridge->OnLiveKitReadySink   = nullptr;
        Bridge->OnLiveKitErrorSink   = nullptr;
        // Idle-reaper sinks.
        Bridge->OnSessionTimeoutSink = nullptr;
        Bridge->OnVoiceTimeoutSink   = nullptr;

        Bridge->Disconnect();
        Bridge.Reset();
    }

    // T-07-08: Clear pending camera requests so stale request_id entries from a
    // prior PIE session cannot match bot_response chunks in the next session.
    PendingCameraRequests.Empty();
    RegisteredCameraComponents.Empty();

    SessionState = EEstuarySessionState::Disconnected;
    Super::Deinitialize();
}

// ============================================================================
// Connect (XPORT-02)
// ============================================================================

void UEstuarySubsystem::Connect(const FString& ServerUrl, const FEstuaryAuthPayload& Auth)
{
    UE_LOG(LogEstuary, Log,
        TEXT("[Subsystem] Connect ENTER — ServerUrl=%s state=%d"),
        *ServerUrl, static_cast<int32>(SessionState));

    check(IsInGameThread());

    // Guard: already in an active or transitioning state — refuse silently.
    if (SessionState == EEstuarySessionState::Connecting ||
        SessionState == EEstuarySessionState::Connected)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Subsystem] Connect called while state=%d — ignoring (Disconnect first)"),
            static_cast<int32>(SessionState));
        return;
    }

    // Body animation (GestureSync) requires audio_sample_rate == 16000 server-side
    // (HuBERT gate; gateway _BODY_ACCEPTED_RATES = (16000,)). If a caller opts
    // into body animation at a different rate, the server silently computes
    // body.active=false and emits ZERO bot_pose frames with no error. Coerce here
    // as a safety net so every Connect path — including hand-built FEstuaryAuthPayload
    // structs that bypass MakeDefaultAuthPayload — behaves correctly.
    FEstuaryAuthPayload NormalizedAuth = Auth;
    if (NormalizedAuth.bEnableBodyAnimation && NormalizedAuth.AudioSampleRate != 16000)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Subsystem] Connect: enable_body_animation=true but audio_sample_rate=%d — "
                 "coercing to 16000 (GestureSync HuBERT gate). Pass 16000 explicitly to silence this."),
            NormalizedAuth.AudioSampleRate);
        NormalizedAuth.AudioSampleRate = 16000;
    }

    // API-key fallback. Same rationale as the sample-rate coercion above: apply it
    // here so EVERY Connect path is covered — including hand-built FEstuaryAuthPayload
    // structs (e.g. the body-animation sample's "Make EstuaryAuthPayload" node) that
    // never route through MakeAuthPayloadFromSettings / GetEffectiveAuthToken. When a
    // caller leaves ApiKey empty, resolve it from the Auth Token project setting, then
    // the ESTUARY_API_KEY environment variable (GetEffectiveAuthToken encapsulates that
    // order). T-09-01-01: never log the key — only whether one was resolved.
    if (NormalizedAuth.ApiKey.IsEmpty())
    {
        NormalizedAuth.ApiKey = UEstuaryDeveloperSettings::GetEffectiveAuthToken();
        UE_LOG(LogEstuary, Log,
            TEXT("[Subsystem] Connect: ApiKey was empty on the auth payload — backfilled from "
                 "Auth Token setting / ESTUARY_API_KEY env var (resolved=%s)."),
            NormalizedAuth.ApiKey.IsEmpty() ? TEXT("still empty — set the field or the env var")
                                            : TEXT("ok"));
    }

    CachedServerUrl     = ServerUrl;
    CachedAuth          = NormalizedAuth;
    ReconnectAttempts   = 0;
    // Explicit Connect() = user intent: clear the session_timeout reconnect
    // suppression so a deliberate resume after an idle reap works. This is the
    // ONLY place the flag is cleared (never on the reap's own transport close).
    bServerEndedSession = false;
    bVoiceSuspended = false;
    CancelReconnect();

    if (!Bridge.IsValid())
    {
        Bridge = MakeShared<FEstuarySocketIOBridge>();
    }

    // ALWAYS rebind — not just on bridge creation. FEstuarySocketIOBridge::
    // Disconnect() nulls every outbound sink (belt-and-suspenders against late
    // marshaled dispatches), and both HandleSessionTimeout and HandleAuthError
    // call it before a later user-initiated Connect() reuses this bridge.
    // Without the rebind, the reconnect authenticates fine but session_info
    // lands in a null sink and the client hangs in Connecting (found live
    // 2026-07-09 on the explicit-resume-after-reap path).
    BindBridgeSinks();

    SetState(EEstuarySessionState::Connecting);
    Bridge->Connect(ServerUrl, CachedAuth);
}

// ============================================================================
// Disconnect
// ============================================================================

void UEstuarySubsystem::Disconnect()
{
    check(IsInGameThread());

    CancelReconnect();

    if (Bridge.IsValid())
    {
        Bridge->Disconnect();
        // Keep Bridge alive across Connect/Disconnect cycles to avoid
        // repeated allocation overhead. Only Deinitialize() resets it.
    }

    SetState(EEstuarySessionState::Disconnected);
}

// ============================================================================
// BindBridgeSinks
// ============================================================================

void UEstuarySubsystem::BindBridgeSinks()
{
    // Pitfall 3: TWeakObjectPtr capture — never raw `this` in lambdas that cross
    // the bridge/game-thread boundary (even though bridge already guarantees GT).
    TWeakObjectPtr<UEstuarySubsystem> WeakThis(this);

    Bridge->OnSessionInfo = [WeakThis](const FEstuarySessionInfo& Info)
    {
        if (UEstuarySubsystem* Self = WeakThis.Get()) { Self->HandleSessionInfo(Info); }
    };

    Bridge->OnAuthError = [WeakThis](const FEstuaryAuthError& Err)
    {
        if (UEstuarySubsystem* Self = WeakThis.Get()) { Self->HandleAuthError(Err); }
    };

    Bridge->OnSessionRejected = [WeakThis](const FEstuarySessionRejected& Rej)
    {
        if (UEstuarySubsystem* Self = WeakThis.Get()) { Self->HandleSessionRejected(Rej); }
    };

    Bridge->OnError = [WeakThis](const FEstuaryError& Err)
    {
        if (UEstuarySubsystem* Self = WeakThis.Get()) { Self->HandleError(Err); }
    };

    Bridge->OnQuotaExceeded = [WeakThis](const FEstuaryQuotaExceeded& Quota)
    {
        if (UEstuarySubsystem* Self = WeakThis.Get()) { Self->HandleQuotaExceeded(Quota); }
    };

    Bridge->OnMemoryUpdated = [WeakThis](const FEstuaryMemoryUpdated& Memory)
    {
        if (UEstuarySubsystem* Self = WeakThis.Get()) { Self->HandleMemoryUpdated(Memory); }
    };

    Bridge->OnClientAction = [WeakThis](const FEstuaryClientAction& Action)
    {
        if (UEstuarySubsystem* Self = WeakThis.Get()) { Self->HandleClientAction(Action); }
    };

    Bridge->OnSessionTimeoutSink = [WeakThis](const FEstuarySessionTimeout& Timeout)
    {
        if (UEstuarySubsystem* Self = WeakThis.Get()) { Self->HandleSessionTimeout(Timeout); }
    };

    Bridge->OnVoiceTimeoutSink = [WeakThis](const FEstuaryVoiceTimeout& Timeout)
    {
        if (UEstuarySubsystem* Self = WeakThis.Get()) { Self->HandleVoiceTimeout(Timeout); }
    };

    Bridge->OnTransportConnected = [WeakThis]()
    {
        if (UEstuarySubsystem* Self = WeakThis.Get()) { Self->HandleTransportConnected(); }
    };

    Bridge->OnTransportClosed = [WeakThis](int32 Code, const FString& Reason)
    {
        if (UEstuarySubsystem* Self = WeakThis.Get()) { Self->HandleTransportClosed(Code, Reason); }
    };

    Bridge->OnTransportError = [WeakThis](const FString& Msg)
    {
        if (UEstuarySubsystem* Self = WeakThis.Get()) { Self->HandleTransportError(Msg); }
    };

    // Phase 3: text-chat event routing.
    Bridge->OnBotResponse = [WeakThis](const FEstuaryBotResponse& Response)
    {
        check(IsInGameThread()); // Bridge guarantees this — the check is a belt-and-suspenders assert.
        if (UEstuarySubsystem* Self = WeakThis.Get())
        {
            Self->HandleBotResponse(Response);
        }
    };

    Bridge->OnInterruptAck = [WeakThis](const FEstuaryInterruptPayload& Payload)
    {
        check(IsInGameThread());
        if (UEstuarySubsystem* Self = WeakThis.Get())
        {
            Self->HandleInterruptAck(Payload);
        }
    };

    // Phase 4: bot_voice routing — broadcast to all registered characters.
    // bot_voice has no character_id on the wire; each AudioPlayerComponent filters
    // by CurrentMessageId (Unity SDK routing pattern).
    Bridge->OnBotVoiceSink = [WeakThis](const FEstuaryBotVoice& Voice)
    {
        check(IsInGameThread()); // Bridge guarantees game-thread dispatch.
        if (UEstuarySubsystem* Self = WeakThis.Get())
        {
            Self->HandleBotVoice(Voice);
        }
    };

    // Phase 5: stt_response routing — route to the component that owns the active voice session.
    Bridge->OnSttResponseSink = [WeakThis](const FEstuarySttResponse& Response)
    {
        check(IsInGameThread()); // Bridge guarantees game-thread dispatch.
        if (UEstuarySubsystem* Self = WeakThis.Get())
        {
            Self->HandleSttResponse(Response);
        }
    };

    // Phase 7: camera_capture routing — route to all registered camera components.
    // OnVisionResponseSink is NOT set on the bridge — vision responses flow via
    // HandleBotResponse detecting a message_id match in PendingCameraRequests.
    Bridge->OnCameraCaptureSink = [WeakThis](const FEstuaryCameraCapture& Capture)
    {
        check(IsInGameThread()); // Bridge guarantees game-thread dispatch.
        if (UEstuarySubsystem* Self = WeakThis.Get())
        {
            Self->HandleCameraCapture(Capture);
        }
    };

    // Phase 6: bot_animation — broadcast to all registered characters (no wire character_id).
    // Terminator frames (IsTerminator()==true) are filtered in HandleBotAnimation;
    // AnimationReceiver never sees them. Per SDK_CONTRACT: sequence==-1 / weights=={} is end-of-stream.
    Bridge->OnBotAnimationSink = [WeakThis](const FEstuaryBotAnimation& Anim)
    {
        check(IsInGameThread()); // Bridge guarantees game-thread dispatch.
        if (UEstuarySubsystem* Self = WeakThis.Get())
        {
            Self->HandleBotAnimation(Anim);
        }
    };

    // Phase 4: bot_pose — broadcast GestureSync body-animation frames to all registered characters
    // (no wire character_id — same broadcast-to-all pattern as bot_animation).
    // Terminator frames (sequence==-1 / bones==[] / is_final==true) are filtered in
    // HandleBotPose; UEstuaryBodyAnimationReceiver never sees them.
    Bridge->OnBotPoseSink = [WeakThis](const FEstuaryBotPose& Pose)
    {
        check(IsInGameThread()); // Bridge guarantees game-thread dispatch.
        if (UEstuarySubsystem* Self = WeakThis.Get())
        {
            Self->HandleBotPose(Pose);
        }
    };

    // -------------------------------------------------------------------
    // Phase 8: LiveKit server event sinks (SDK_CONTRACT §voice_livekit).
    // Bridge fires these on the game thread (getnamo marshals internally).
    // -------------------------------------------------------------------

    // Server → Client: livekit_token {token, url, room}
    // Fires when the server responds to our livekit_token request.
    Bridge->OnLiveKitTokenSink = [WeakThis](const FString& Token, const FString& Url, const FString& Room)
    {
        check(IsInGameThread());
        if (UEstuarySubsystem* Self = WeakThis.Get())
        {
            Self->HandleLiveKitTokenReceived(Token, Url, Room);
        }
    };

    // Server → Client: livekit_ready {room}
    // Fires when the server is ready for the client to join the room.
    Bridge->OnLiveKitReadySink = [WeakThis](const FString& Room)
    {
        check(IsInGameThread());
        if (UEstuarySubsystem* Self = WeakThis.Get())
        {
            Self->HandleLiveKitReady(Room);
        }
    };

    // Server → Client: livekit_error {error}
    // Fires when the server signals a LiveKit-side error.
    Bridge->OnLiveKitErrorSink = [WeakThis](const FString& ErrorMsg)
    {
        check(IsInGameThread());
        if (UEstuarySubsystem* Self = WeakThis.Get())
        {
            Self->HandleLiveKitError(ErrorMsg);
        }
    };
}

// ============================================================================
// Bridge sink handlers
// ============================================================================

void UEstuarySubsystem::HandleSessionInfo(const FEstuarySessionInfo& Info)
{
    check(IsInGameThread());

    SessionInfo       = Info;
    ReconnectAttempts = 0;  // success resets backoff counter

    // Surface silent animation-gate failures (SDK_CONTRACT §session_info.animation_capabilities).
    // If the client opted into a modality but the server's runtime AND-gate is
    // closed, the server emits ZERO frames for it with no error event. Without this
    // diagnostic the symptom is an invisible "the character never animates".
    if (Info.AnimationCapabilities.bPresent)
    {
        if (CachedAuth.bEnableBodyAnimation && !Info.AnimationCapabilities.Body.bActive)
        {
            UE_LOG(LogEstuary, Warning,
                TEXT("[Subsystem] enable_body_animation was requested but server reports body.active=false "
                     "(provider='%s'). NO bot_pose frames will arrive. Likely causes: audio_sample_rate != 16000, "
                     "server GestureSync inference disabled, or this character lacks animation_capabilities.body.enabled."),
                *Info.AnimationCapabilities.Body.Provider);
        }
        if (CachedAuth.bEnableAnimation && !Info.AnimationCapabilities.Face.bActive)
        {
            UE_LOG(LogEstuary, Warning,
                TEXT("[Subsystem] enable_animation (face) was requested but server reports face.active=false "
                     "(provider='%s'). NO bot_animation frames will arrive. Likely causes: server ENABLE_A2F disabled "
                     "or this character lacks animation_capabilities.face.enabled."),
                *Info.AnimationCapabilities.Face.Provider);
        }
    }

    SetState(EEstuarySessionState::Connected);

    UE_LOG(LogEstuary, Log,
        TEXT("[Subsystem] session_info received: session_id=%s conversation_id=%s char=%s player=%s body_active=%d face_active=%d"),
        *Info.SessionId, *Info.ConversationId, *Info.CharacterId, *Info.PlayerId,
        Info.AnimationCapabilities.Body.bActive ? 1 : 0,
        Info.AnimationCapabilities.Face.bActive ? 1 : 0);

    OnSessionConnected.Broadcast(Info);  // Pitfall 17 — on game thread via check above
}

void UEstuarySubsystem::HandleAuthError(const FEstuaryAuthError& Err)
{
    check(IsInGameThread());

    UE_LOG(LogEstuary, Warning, TEXT("[Subsystem] auth_error: %s"), *Err.Error);

    SetState(EEstuarySessionState::Error);
    OnAuthError.Broadcast(Err);

    // Auth failure is terminal — do NOT reconnect. The consumer must fix the
    // credentials and call Connect() again with a corrected FEstuaryAuthPayload.
    if (Bridge.IsValid()) { Bridge->Disconnect(); }
}

void UEstuarySubsystem::HandleSessionRejected(const FEstuarySessionRejected& Rej)
{
    check(IsInGameThread());

    UE_LOG(LogEstuary, Warning,
        TEXT("[Subsystem] session_rejected: reason=%s cap=%d share_token_id=%s"),
        *Rej.Reason, Rej.Cap, *Rej.ShareTokenId);

    SetState(EEstuarySessionState::Error);
    OnSessionRejected.Broadcast(Rej);

    // Same as auth_error: do not auto-reconnect. Policy rejections require
    // user action (wait for concurrent sessions to end, or use a different token).
    if (Bridge.IsValid()) { Bridge->Disconnect(); }
}

void UEstuarySubsystem::HandleError(const FEstuaryError& Err)
{
    check(IsInGameThread());

    UE_LOG(LogEstuary, Warning, TEXT("[Subsystem] error: %s"), *Err.Message);

    // Non-terminal (SDK_CONTRACT §error): the gateway does NOT disconnect. Surface the
    // payload only — no SetState, no Disconnect (distinct from auth_error/session_rejected).
    OnError.Broadcast(Err);
}

void UEstuarySubsystem::HandleQuotaExceeded(const FEstuaryQuotaExceeded& Quota)
{
    check(IsInGameThread());

    UE_LOG(LogEstuary, Warning,
        TEXT("[Subsystem] quota_exceeded: error=%s current=%d limit=%d remaining=%d tier=%s"),
        *Quota.ErrorCode, Quota.Current, Quota.Limit, Quota.Remaining, *Quota.Tier);

    OnQuotaExceeded.Broadcast(Quota);

    // The gateway disconnects immediately after emitting quota_exceeded. We do NOT call
    // Bridge->Disconnect() here: the imminent transport close drives the state machine
    // (HandleTransportClosed), preserving the existing reconnect-policy semantics. Calling
    // SetState(Error) here would race that path; let the transport close be the source of truth.
}

void UEstuarySubsystem::HandleMemoryUpdated(const FEstuaryMemoryUpdated& Memory)
{
    check(IsInGameThread());

    UE_LOG(LogEstuary, Log,
        TEXT("[Subsystem] memory_updated: agent=%s player=%s extracted=%d facts=%d new=%d"),
        *Memory.AgentId, *Memory.PlayerId, Memory.MemoriesExtracted, Memory.FactsExtracted,
        Memory.NewMemories.Num());

    OnMemoryUpdated.Broadcast(Memory);
}

void UEstuarySubsystem::HandleSessionTimeout(const FEstuarySessionTimeout& Timeout)
{
    check(IsInGameThread());

    UE_LOG(LogEstuary, Warning,
        TEXT("[Subsystem] session_timeout: reason=%s idle=%ds timeout=%ds — server reaped the session; auto-reconnect suppressed"),
        *Timeout.Reason, Timeout.IdleSeconds, Timeout.TimeoutSeconds);

    // Suppress the auto-reconnect for the server-initiated disconnect that
    // immediately follows. Reconnecting would re-auth and re-establish billed
    // voice resources (LiveKit pre-join, Deepgram) with nobody talking, in a
    // loop (SDK_CONTRACT §session_timeout). Cleared only by explicit Connect().
    bServerEndedSession = true;

    // Race guard: this event and the transport close are marshaled to the game
    // thread independently — the close can land FIRST and already have scheduled
    // a backoff reconnect. Cancel it and settle in Disconnected.
    if (SessionState == EEstuarySessionState::Reconnecting)
    {
        UE_LOG(LogEstuary, Log,
            TEXT("[Subsystem] session_timeout arrived after the transport close — cancelling the scheduled reconnect"));
        CancelReconnect();
        SetState(EEstuarySessionState::Disconnected);
    }

    // Release local voice state now (mic off, LiveKit room disposed client-side —
    // the server has already deleted it).
    ReleaseVoiceResourcesLocal(TEXT("session_timeout"));

    OnSessionTimeout.Broadcast(Timeout);

    // Proactively tear the socket down — do NOT wait for a transport close.
    // getnamo's OnDisconnectedCallback is TRANSPORT-level and does NOT fire when
    // the server disconnects only the /sdk NAMESPACE (verified live 2026-07-09:
    // after a gateway reap the Engine.IO connection lingered and the client sat
    // "Connected" indefinitely — unlike the JS/Python socket.io clients, which
    // surface per-namespace disconnects). Bridge->Disconnect() never fires
    // OnTransportClosed, so this cannot cause a Reconnecting transition; the
    // bServerEndedSession guards above remain for a real close racing in first.
    if (Bridge.IsValid())
    {
        Bridge->Disconnect();
    }
    SetState(EEstuarySessionState::Disconnected);
}

void UEstuarySubsystem::HandleVoiceTimeout(const FEstuaryVoiceTimeout& Timeout)
{
    check(IsInGameThread());

    UE_LOG(LogEstuary, Warning,
        TEXT("[Subsystem] voice_timeout: reason=%s idle=%ds timeout=%ds — releasing local voice; socket stays connected"),
        *Timeout.Reason, Timeout.IdleSeconds, Timeout.TimeoutSeconds);

    // Voice lane only: STT closed and LiveKit room deleted server-side, but the
    // Socket.IO session is alive and text chat continues. Deliberately does NOT
    // touch bServerEndedSession — no disconnect follows (opposite of
    // session_timeout; SDK_CONTRACT §voice_timeout Ordering).
    ReleaseVoiceResourcesLocal(TEXT("voice_timeout"));

    bVoiceSuspended = true; // cleared on livekit_ready (re-establishment) or Connect()

    // Recommended app UX (auto-mute illusion): keep the call UI open, show the
    // mic as muted, and StartListening() again when the user unmutes — that
    // starts a FRESH voice session against the server.
    OnVoiceTimeout.Broadcast(Timeout);
}

void UEstuarySubsystem::ReleaseVoiceResourcesLocal(const TCHAR* Context)
{
    check(IsInGameThread());

    bool bReleasedAny = false;

    // Stop mic capture WITHOUT emitting stop_voice — the server-side voice
    // session is already gone; streaming into a closed STT stream (or emitting
    // stop_voice for it) is the stale-voice-state bug class that bit the TS,
    // Python, and Lens SDKs. Sweep every registered character's sibling mic
    // component: StopCaptureLocal() is an idempotent no-op when not capturing.
    for (auto& Pair : RegisteredCharacters)
    {
        UEstuaryCharacterComponent* Comp = Pair.Value.Get();
        if (!Comp) { continue; }

        AActor* Owner = Comp->GetOwner();
        if (!Owner) { continue; }

        if (UEstuaryMicrophoneComponent* Mic = Owner->FindComponentByClass<UEstuaryMicrophoneComponent>())
        {
            if (Mic->bIsCapturing)
            {
                Mic->StopCaptureLocal();
                bReleasedAny = true;
            }
        }

        // Clear playback message tracking too (stale-CurrentMessageId latch,
        // found live 2026-07-09): in LiveKit mode EnqueueBotVoice tracks the
        // message id but skips PCM, so nothing ever drains and the id never
        // resets — after the transition to WebSocket voice every new
        // bot_voice message mismatches the latched id and ALL audio is
        // silently dropped as "stale". An interrupt for a different id
        // cannot clear it (the mismatch guard is intentional, T-04-03-01).
        // Unconditional flush on release resets the latch; audio for any
        // in-flight message is already moot — its transport just died.
        if (UEstuaryAudioPlayerComponent* Player = Owner->FindComponentByClass<UEstuaryAudioPlayerComponent>())
        {
            Player->FlushAllForMessage(FString());
            bReleasedAny = true;
        }
    }

    // Dispose the LiveKit room client-side WITHOUT emitting livekit_leave — the
    // room is already deleted server-side, and the TS SDK's releaseVoice()
    // (canonical mirror) emits nothing either. The room's own Disconnected event
    // during this teardown is EXPECTED and not a call failure.
    // CurrentVoiceMode reverts to WebSocket in OnLiveKitVoiceLeft.
    if (CurrentVoiceMode == EEstuaryVoiceMode::LiveKit &&
        LiveKitBridge.IsAvailable() && LiveKitBridge.GetVoice() &&
        LiveKitBridge.GetVoice()->IsJoined())
    {
        LiveKitBridge.GetVoice()->LeaveRoom();
        bReleasedAny = true;
    }

    if (!ActiveVoiceCharacterId.IsEmpty())
    {
        ActiveVoiceCharacterId.Empty();
        bReleasedAny = true;
    }

    if (bReleasedAny)
    {
        UE_LOG(LogEstuary, Log,
            TEXT("[Subsystem] ReleaseVoiceResourcesLocal (%s): local voice state released — no server emits"),
            Context);
    }
    else
    {
        UE_LOG(LogEstuary, Verbose,
            TEXT("[Subsystem] ReleaseVoiceResourcesLocal (%s): nothing to release"), Context);
    }
}

void UEstuarySubsystem::HandleTransportConnected()
{
    check(IsInGameThread());

    // Transport is open; bridge has already emitted `authenticate`.
    // State remains Connecting until session_info arrives — no state change here.
    UE_LOG(LogEstuary, Verbose, TEXT("[Subsystem] transport connected — awaiting session_info"));
}

void UEstuarySubsystem::HandleTransportClosed(int32 CloseCode, const FString& Reason)
{
    check(IsInGameThread());

    UE_LOG(LogEstuary, Log,
        TEXT("[Subsystem] transport closed code=%d reason=%s"), CloseCode, *Reason);

    // ANY transport close of a voice-active session leaves stale local voice
    // state: the server deletes the LiveKit room on ANY disconnect (SDK_CONTRACT
    // §session_timeout Related behavior) and the STT stream dies with the socket.
    // Release locally (no server emits) so the mic is not left hot and the next
    // voice start after a reconnect starts a FRESH session instead of streaming
    // into a closed stream. Idempotent — no-op when voice was never active.
    ReleaseVoiceResourcesLocal(TEXT("transport closed"));

    // Server-initiated idle reap (session_timeout was received just before this
    // close): do NOT auto-reconnect. Re-authenticating would re-establish billed
    // voice resources with nobody talking, in a loop. Explicit Connect() (user
    // intent) is the only resume path — it clears bServerEndedSession.
    if (bServerEndedSession)
    {
        UE_LOG(LogEstuary, Log,
            TEXT("[Subsystem] transport closed after session_timeout — auto-reconnect suppressed (explicit Connect() required)"));
        CancelReconnect();
        SetState(EEstuarySessionState::Disconnected);
        return;
    }

    // 1000 = clean intentional close (Disconnect() was called) or we're already in
    // an Error state (auth_error / session_rejected path already handled cleanup).
    // In either case: move to Disconnected and do nothing more.
    if (CloseCode == 1000 || SessionState == EEstuarySessionState::Error)
    {
        SetState(EEstuarySessionState::Disconnected);
        return;
    }

    // Non-clean close: transition to Reconnecting and start the backoff timer.
    // XPORT-06: code 1011 = gateway sticky-session failure -> fresh session needed.
    // Code 1006 = network drop. Both trigger reconnect.
    SetState(EEstuarySessionState::Reconnecting);
    ScheduleReconnect();
}

void UEstuarySubsystem::HandleTransportError(const FString& Message)
{
    check(IsInGameThread());

    UE_LOG(LogEstuary, Warning, TEXT("[Subsystem] transport error: %s"), *Message);

    // Transport errors are distinct from auth/policy errors — do not broadcast
    // OnAuthError or OnSessionRejected. Surface via state change only; consumer
    // can observe via OnStateChanged binding.
    SetState(EEstuarySessionState::Error);
}

// ============================================================================
// SetState — single gate for all transitions
// ============================================================================

void UEstuarySubsystem::SetState(EEstuarySessionState NewState)
{
    if (SessionState == NewState) { return; }

    const EEstuarySessionState Prev = SessionState;
    SessionState = NewState;

    UE_LOG(LogEstuary, Verbose,
        TEXT("[Subsystem] state %d -> %d"),
        static_cast<int32>(Prev), static_cast<int32>(NewState));

    OnStateChanged.Broadcast(NewState);  // Pitfall 17 — always on game thread
}

// ============================================================================
// Reconnect with exponential backoff (XPORT-06)
// ============================================================================

void UEstuarySubsystem::ScheduleReconnect()
{
    check(IsInGameThread());
    CancelReconnect();

    // Exponential backoff capped at 30s: 2^AttemptIdx, max 30.
    // AttemptIdx is clamped so 2^5 = 32 -> clamp to 30.
    const int32 AttemptIdx = FMath::Min(ReconnectAttempts, 5);
    const float DelaySec   = FMath::Min(FMath::Pow(2.0f, static_cast<float>(AttemptIdx)), 30.0f);
    ReconnectAttempts++;

    UE_LOG(LogEstuary, Log,
        TEXT("[Subsystem] reconnect scheduled in %.1fs (attempt %d)"),
        DelaySec, ReconnectAttempts);

    TWeakObjectPtr<UEstuarySubsystem> WeakThis(this);
    ReconnectHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([WeakThis](float DeltaTime) -> bool
        {
            if (UEstuarySubsystem* Self = WeakThis.Get())
            {
                return Self->TickReconnect(DeltaTime);  // returns false = one-shot
            }
            return false;  // subsystem gone — cancel the ticker
        }),
        DelaySec);
}

void UEstuarySubsystem::CancelReconnect()
{
    if (ReconnectHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(ReconnectHandle);
        ReconnectHandle.Reset();
    }
}

bool UEstuarySubsystem::TickReconnect(float /*DeltaTime*/)
{
    ReconnectHandle.Reset();  // one-shot; clear the handle before any early-out
    check(IsInGameThread());

    // Belt-and-braces for the session_timeout race: if the reap event was
    // processed AFTER the transport close scheduled this ticker, the flag is set
    // now — abort instead of re-establishing a billed session in a loop.
    if (bServerEndedSession)
    {
        UE_LOG(LogEstuary, Log,
            TEXT("[Subsystem] TickReconnect: server ended the session (session_timeout) — aborting scheduled reconnect"));
        SetState(EEstuarySessionState::Disconnected);
        return false;
    }

    // Guard: state may have changed since the ticker was scheduled
    // (e.g., consumer called Disconnect() while backoff was pending).
    if (SessionState != EEstuarySessionState::Reconnecting)
    {
        UE_LOG(LogEstuary, Verbose,
            TEXT("[Subsystem] TickReconnect: state changed to %d before timer fired — aborting"),
            static_cast<int32>(SessionState));
        return false;
    }

    UE_LOG(LogEstuary, Log,
        TEXT("[Subsystem] reconnecting attempt %d to %s"),
        ReconnectAttempts, *CachedServerUrl);

    // Cycle the bridge: always create a FRESH FEstuarySocketIOBridge + new socket.
    // Pitfall 12: never attempt to resume the old session_id — gateway expects
    // a completely new Socket.IO connection (new socket handshake + new authenticate).
    if (Bridge.IsValid())
    {
        Bridge->OnSessionInfo        = nullptr;
        Bridge->OnAuthError          = nullptr;
        Bridge->OnSessionRejected    = nullptr;
        Bridge->OnError              = nullptr;
        Bridge->OnQuotaExceeded      = nullptr;
        Bridge->OnMemoryUpdated      = nullptr;
        Bridge->OnTransportConnected = nullptr;
        Bridge->OnTransportClosed    = nullptr;
        Bridge->OnTransportError     = nullptr;
        Bridge->OnBotResponse        = nullptr;
        Bridge->OnInterruptAck       = nullptr;
        Bridge->OnBotVoiceSink       = nullptr;
        Bridge->OnSttResponseSink    = nullptr;
        Bridge->OnCameraCaptureSink  = nullptr;
        Bridge->OnBotAnimationSink   = nullptr;
        Bridge->OnBotPoseSink        = nullptr;
        // Phase 8: clear LiveKit sinks on reconnect cycle.
        Bridge->OnLiveKitTokenSink   = nullptr;
        Bridge->OnLiveKitReadySink   = nullptr;
        Bridge->OnLiveKitErrorSink   = nullptr;
        // Idle-reaper sinks.
        Bridge->OnSessionTimeoutSink = nullptr;
        Bridge->OnVoiceTimeoutSink   = nullptr;
        Bridge->Disconnect();
    }

    Bridge = MakeShared<FEstuarySocketIOBridge>();
    BindBridgeSinks();

    SetState(EEstuarySessionState::Connecting);
    Bridge->Connect(CachedServerUrl, CachedAuth);

    return false;  // one-shot ticker; do not repeat
}

// ============================================================================
// HandleBotVoice (AUDIO-01)
// ============================================================================

void UEstuarySubsystem::HandleBotVoice(const FEstuaryBotVoice& Voice)
{
    check(IsInGameThread());

    // bot_voice has no character_id on the wire (SDK_CONTRACT §text_chat).
    // Broadcast to ALL registered characters; each UEstuaryAudioPlayerComponent
    // filters by CurrentMessageId to ignore stale-message chunks.
    // In single-character scenes this reaches exactly one component.
    // In multi-character scenes each component self-routes (Unity SDK pattern).
    for (auto& Pair : RegisteredCharacters)
    {
        if (UEstuaryCharacterComponent* Comp = Pair.Value.Get())
        {
            Comp->HandleBotVoiceFromSubsystem(Voice);
        }
    }
}

// ============================================================================
// EmitAudioPlaybackComplete (AUDIO-05)
// ============================================================================

void UEstuarySubsystem::EmitAudioPlaybackComplete(const FEstuaryAudioPlaybackComplete& Payload)
{
    check(IsInGameThread());

    if (!Bridge.IsValid() || !Bridge->IsConnected())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Subsystem] EmitAudioPlaybackComplete: not connected — drop (message_id=%s)"),
            *Payload.MessageId);
        return;
    }

    Bridge->EmitAudioPlaybackComplete(Payload);
}

// ============================================================================
// Character registry (Plan 03-03; TEXT-06)
// ============================================================================

void UEstuarySubsystem::RegisterCharacter(UEstuaryCharacterComponent* Component)
{
    check(IsInGameThread()); // Pitfall 17: registry is game-thread-only.

    if (!IsValid(Component))
    {
        UE_LOG(LogEstuary, Warning, TEXT("[Subsystem] RegisterCharacter: null or invalid component — ignored"));
        return;
    }

    const FString CharacterId = Component->GetCharacterId();

    if (CharacterId.IsEmpty())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Subsystem] RegisterCharacter: component '%s' has empty CharacterId — cannot route events"),
            *GetNameSafe(Component));
        return;
    }

    RegisteredCharacters.Add(CharacterId, TWeakObjectPtr<UEstuaryCharacterComponent>(Component));
    UE_LOG(LogEstuary, Log, TEXT("[Subsystem] RegisterCharacter: '%s' registered (total=%d)"),
        *CharacterId, RegisteredCharacters.Num());
}

void UEstuarySubsystem::UnregisterCharacter(UEstuaryCharacterComponent* Component)
{
    check(IsInGameThread());

    if (!Component) { return; } // nullptr guard — do not call IsValid on null raw ptr

    const FString CharacterId = Component->GetCharacterId();
    if (CharacterId.IsEmpty()) { return; }

    // Only remove if the registered entry still points to THIS component
    // (not a replacement that was registered under the same CharacterId).
    TWeakObjectPtr<UEstuaryCharacterComponent>* Existing = RegisteredCharacters.Find(CharacterId);
    if (Existing && Existing->Get() == Component)
    {
        RegisteredCharacters.Remove(CharacterId);
        UE_LOG(LogEstuary, Log, TEXT("[Subsystem] UnregisterCharacter: '%s' unregistered (total=%d)"),
            *CharacterId, RegisteredCharacters.Num());
    }
}

// ============================================================================
// Text-chat forwarding (Plan 03-03; TEXT-02, TEXT-04, TEXT-05)
// ============================================================================

void UEstuarySubsystem::SendUserText(const FEstuaryUserText& Payload)
{
    check(IsInGameThread());
    if (!Bridge.IsValid() || !Bridge->IsConnected())
    {
        UE_LOG(LogEstuary, Warning, TEXT("[Subsystem] SendUserText: not connected — drop (message_id=%s)"),
            *Payload.MessageId);
        return;
    }
    Bridge->EmitUserText(Payload);
}

void UEstuarySubsystem::SendSayLine(const FString& Text, bool bTextOnly)
{
    check(IsInGameThread());
    if (!Bridge.IsValid() || !Bridge->IsConnected())
    {
        UE_LOG(LogEstuary, Warning, TEXT("[Subsystem] SendSayLine: not connected — drop"));
        return;
    }
    Bridge->EmitSayLine(Text, bTextOnly);
}

void UEstuarySubsystem::SendInterrupt(const FEstuaryInterruptPayload& Payload)
{
    check(IsInGameThread());
    if (!Bridge.IsValid() || !Bridge->IsConnected())
    {
        UE_LOG(LogEstuary, Warning, TEXT("[Subsystem] SendInterrupt: not connected — drop"));
        return;
    }
    Bridge->EmitInterrupt(Payload);
}

void UEstuarySubsystem::SendUpdatePreferences(const FEstuaryPreferences& Payload)
{
    check(IsInGameThread());
    if (!Bridge.IsValid() || !Bridge->IsConnected())
    {
        UE_LOG(LogEstuary, Warning, TEXT("[Subsystem] SendUpdatePreferences: not connected — drop"));
        return;
    }
    Bridge->EmitUpdatePreferences(Payload);
}

// ============================================================================
// Bot response + interrupt routing (Plan 03-03)
// ============================================================================

void UEstuarySubsystem::HandleBotResponse(const FEstuaryBotResponse& InResponse)
{
    check(IsInGameThread());

    // Populate CharacterId from session (not present on wire; client-side routing field).
    FEstuaryBotResponse Response = InResponse;
    Response.CharacterId = SessionInfo.CharacterId;

    // Route to the registered component matching this session's character_id.
    TWeakObjectPtr<UEstuaryCharacterComponent>* Weak = RegisteredCharacters.Find(Response.CharacterId);
    if (!Weak || !Weak->IsValid())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Subsystem] HandleBotResponse: no registered component for CharacterId='%s' (message_id=%s)"),
            *Response.CharacterId, *Response.MessageId);
        return;
    }

    if (UEstuaryCharacterComponent* Comp = Weak->Get())
    {
        Comp->HandleBotResponseFromSubsystem(Response);
    }

    // -----------------------------------------------------------------------
    // Vision response assembly (CAM-06):
    // If this bot_response's message_id matches a pending camera request_id,
    // assemble FEstuaryVisionResponse and dispatch to the character component.
    // T-07-08: Remove entry on final chunk to prevent unbounded map growth.
    // -----------------------------------------------------------------------
    if (PendingCameraRequests.Contains(Response.MessageId))
    {
        const FString CameraCharacterId = PendingCameraRequests[Response.MessageId];

        TWeakObjectPtr<UEstuaryCharacterComponent>* CamCompPtr =
            RegisteredCharacters.Find(CameraCharacterId);
        if (CamCompPtr && CamCompPtr->IsValid())
        {
            FEstuaryVisionResponse VR;
            VR.MessageId   = Response.MessageId;
            VR.CharacterId = CameraCharacterId;
            VR.Description = Response.Text;
            VR.bIsFinal    = Response.bIsFinal;

            CamCompPtr->Get()->HandleVisionResponseFromSubsystem(VR);
        }

        // Clean up the pending entry once the final chunk arrives (T-07-08).
        if (Response.bIsFinal)
        {
            PendingCameraRequests.Remove(Response.MessageId);
            UE_LOG(LogEstuary, Verbose,
                TEXT("[Subsystem] VLM response complete: request_id=%s char='%s'"),
                *Response.MessageId, *CameraCharacterId);
        }
    }
}

void UEstuarySubsystem::HandleClientAction(const FEstuaryClientAction& InAction)
{
    check(IsInGameThread());

    // Populate CharacterId from session (not present on wire; client-side routing
    // field — same pattern as HandleBotResponse).
    FEstuaryClientAction Action = InAction;
    Action.CharacterId = SessionInfo.CharacterId;

    UE_LOG(LogEstuary, Log,
        TEXT("[Subsystem] client_action: name=%s args=%d char='%s' message_id=%s"),
        *Action.Name, Action.Arguments.Num(), *Action.CharacterId, *Action.MessageId);

    // Route to the registered component matching this session's character_id.
    TWeakObjectPtr<UEstuaryCharacterComponent>* Weak = RegisteredCharacters.Find(Action.CharacterId);
    if (!Weak || !Weak->IsValid())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Subsystem] HandleClientAction: no registered component for CharacterId='%s' (action=%s message_id=%s)"),
            *Action.CharacterId, *Action.Name, *Action.MessageId);
        return;
    }

    if (UEstuaryCharacterComponent* Comp = Weak->Get())
    {
        Comp->HandleClientActionFromSubsystem(Action);
    }
}

void UEstuarySubsystem::HandleInterruptAck(const FEstuaryInterruptPayload& InPayload)
{
    check(IsInGameThread());

    FEstuaryInterruptPayload Payload = InPayload;
    Payload.CharacterId = SessionInfo.CharacterId;

    TWeakObjectPtr<UEstuaryCharacterComponent>* Weak = RegisteredCharacters.Find(Payload.CharacterId);
    if (!Weak || !Weak->IsValid())
    {
        // No component to route to — may happen before BeginPlay completes.
        UE_LOG(LogEstuary, Verbose,
            TEXT("[Subsystem] HandleInterruptAck: no registered component for CharacterId='%s'"),
            *Payload.CharacterId);
        return;
    }

    if (UEstuaryCharacterComponent* Comp = Weak->Get())
    {
        Comp->HandleInterruptAckFromSubsystem(Payload);
    }
}

// ============================================================================
// Voice/STT API — Phase 5 (MIC-03, MIC-04)
// ============================================================================

void UEstuarySubsystem::SendStartVoice()
{
    check(IsInGameThread());
    StartVoiceSession(FString());
}

void UEstuarySubsystem::SendStreamAudio(const FEstuaryStreamAudio& Payload)
{
    check(IsInGameThread());
    if (!Bridge.IsValid() || !Bridge->IsConnected())
    {
        UE_LOG(LogEstuary, Warning, TEXT("[Subsystem] SendStreamAudio: not connected — drop"));
        return;
    }
    Bridge->EmitStreamAudio(Payload);
}

void UEstuarySubsystem::SendStopVoice()
{
    check(IsInGameThread());
    StopVoiceSession(FString());
}

void UEstuarySubsystem::StartVoiceSession(const FString& CharacterId)
{
    check(IsInGameThread());
    if (!Bridge.IsValid() || !Bridge->IsConnected())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Subsystem] StartVoiceSession: not connected (char='%s')"), *CharacterId);
        return;
    }
    ActiveVoiceCharacterId = CharacterId;
    Bridge->EmitStartVoice();
    UE_LOG(LogEstuary, Log,
        TEXT("[Subsystem] StartVoiceSession: start_voice emitted (char='%s')"), *CharacterId);

    // Voice start = voice intent: (re)join LiveKit when it is available but not
    // active. Two reasons (SDK_CONTRACT §voice_livekit / §voice_timeout):
    //  1. Warm start — the gateway allocates voice resources lazily on the
    //     livekit_token request, so emitting it here pre-warms the bot join.
    //  2. Auto-mute resume parity — after a voice_timeout released the room,
    //     the next voice start must transparently begin a FRESH LiveKit session
    //     (livekit_token → livekit_join) instead of silently degrading to
    //     WebSocket voice (no AEC, PTT/Flux end-of-turn race). Matches the TS
    //     SDK's unmute behavior. RequestLiveKitToken self-guards: no-op when
    //     LiveKit is disabled, unavailable, or already joined; WebSocket voice
    //     keeps flowing until the room takes over.
    if (CurrentVoiceMode == EEstuaryVoiceMode::WebSocket)
    {
        RequestLiveKitToken();
    }
}

void UEstuarySubsystem::StopVoiceSession(const FString& CharacterId)
{
    check(IsInGameThread());
    if (!Bridge.IsValid())
    {
        ActiveVoiceCharacterId.Empty();
        return;
    }
    if (Bridge->IsConnected())
    {
        Bridge->EmitStopVoice();
    }
    ActiveVoiceCharacterId.Empty();
    UE_LOG(LogEstuary, Log,
        TEXT("[Subsystem] StopVoiceSession: stop_voice emitted (char='%s')"), *CharacterId);
}

void UEstuarySubsystem::HandleSttResponse(const FEstuarySttResponse& Response)
{
    check(IsInGameThread());

    // Populate the client-side routing field before dispatch.
    FEstuarySttResponse Routed = Response;
    Routed.CharacterId = ActiveVoiceCharacterId;

    if (!ActiveVoiceCharacterId.IsEmpty())
    {
        // Route to the specific character that owns the active voice session.
        if (TWeakObjectPtr<UEstuaryCharacterComponent>* Found =
                RegisteredCharacters.Find(ActiveVoiceCharacterId))
        {
            if (UEstuaryCharacterComponent* Comp = Found->Get())
            {
                Comp->HandleSttResponseFromSubsystem(Routed);
                return;
            }
        }
    }

    // Fallback: no ActiveVoiceCharacterId or component not found.
    // Broadcast to all registered components (single-character scenes).
    for (auto& Pair : RegisteredCharacters)
    {
        if (UEstuaryCharacterComponent* Comp = Pair.Value.Get())
        {
            Comp->HandleSttResponseFromSubsystem(Routed);
        }
    }
}

// ============================================================================
// EmitCameraImage (Phase 7 — CAM-01, CAM-02, CAM-06)
// ============================================================================

void UEstuarySubsystem::EmitCameraImage(const FEstuaryCameraImage& Payload)
{
    check(IsInGameThread());

    if (!Bridge.IsValid() || !Bridge->IsConnected())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Subsystem] EmitCameraImage: not connected — drop (request_id=%s)"),
            *Payload.RequestId);
        return;
    }

    // Store request_id → character_id correlation for VLM response routing (CAM-06).
    // T-07-08: entry will be removed in HandleBotResponse when the final chunk arrives.
    if (!Payload.RequestId.IsEmpty() && !Payload.CharacterId.IsEmpty())
    {
        PendingCameraRequests.Add(Payload.RequestId, Payload.CharacterId);
        UE_LOG(LogEstuary, Verbose,
            TEXT("[Subsystem] EmitCameraImage: pending request_id=%s char='%s' (pending=%d)"),
            *Payload.RequestId, *Payload.CharacterId, PendingCameraRequests.Num());
    }

    Bridge->EmitCameraImage(Payload);
}

// ============================================================================
// EmitUserPos — GestureSync dyadic conditioning (paper p_y)
// SDK_CONTRACT §user_pos — REQUIRED-FOR-BODY when GestureSync provider is active.
// Hot-path (~30 Hz): no per-frame logs; silent drop on disconnect.
// ============================================================================

void UEstuarySubsystem::EmitUserPos(float X, float Y)
{
    check(IsInGameThread());
    if (!Bridge.IsValid() || !Bridge->IsConnected())
    {
        return;
    }
    Bridge->EmitUserPos(X, Y);
}

// ============================================================================
// RegisterCameraComponent / UnregisterCameraComponent (Phase 7 — CAM-03)
// ============================================================================

void UEstuarySubsystem::RegisterCameraComponent(UEstuaryCameraComponent* Component)
{
    check(IsInGameThread());

    if (!IsValid(Component))
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Subsystem] RegisterCameraComponent: null or invalid component — ignored."));
        return;
    }

    const FString CharacterId = Component->GetCharacterId();
    if (CharacterId.IsEmpty())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Subsystem] RegisterCameraComponent: component has empty CharacterId — cannot route camera_capture."));
        return;
    }

    RegisteredCameraComponents.Add(CharacterId, TWeakObjectPtr<UEstuaryCameraComponent>(Component));

    // CAM-07: keep a freshly-registered camera in sync with the current mode.
    Component->SetCaptureSource(DesiredCaptureSource);

    UE_LOG(LogEstuary, Log,
        TEXT("[Subsystem] RegisterCameraComponent: '%s' registered (total=%d)"),
        *CharacterId, RegisteredCameraComponents.Num());
}

void UEstuarySubsystem::UnregisterCameraComponent(UEstuaryCameraComponent* Component)
{
    check(IsInGameThread());

    if (!Component) { return; }

    const FString CharacterId = Component->GetCharacterId();
    if (CharacterId.IsEmpty()) { return; }

    // Only remove if the registered entry still points to THIS component.
    TWeakObjectPtr<UEstuaryCameraComponent>* Existing =
        RegisteredCameraComponents.Find(CharacterId);
    if (Existing && Existing->Get() == Component)
    {
        RegisteredCameraComponents.Remove(CharacterId);
        UE_LOG(LogEstuary, Log,
            TEXT("[Subsystem] UnregisterCameraComponent: '%s' unregistered (total=%d)"),
            *CharacterId, RegisteredCameraComponents.Num());
    }
}

// ============================================================================
// Capture source mode (CAM-07) — toggle / set / label for the VLM camera
// ============================================================================

EEstuaryCaptureSource UEstuarySubsystem::ToggleCaptureSource()
{
    check(IsInGameThread());

    const EEstuaryCaptureSource NewSource =
        (DesiredCaptureSource == EEstuaryCaptureSource::Webcam)
            ? EEstuaryCaptureSource::RenderedScene
            : EEstuaryCaptureSource::Webcam;

    SetCaptureSourceAll(NewSource);
    return NewSource;
}

void UEstuarySubsystem::SetCaptureSourceAll(EEstuaryCaptureSource NewSource)
{
    check(IsInGameThread());

    DesiredCaptureSource = NewSource;

    int32 Applied = 0;
    for (auto& KVP : RegisteredCameraComponents)
    {
        if (UEstuaryCameraComponent* Comp = KVP.Value.Get())
        {
            Comp->SetCaptureSource(NewSource);
            ++Applied;
        }
    }

    UE_LOG(LogEstuary, Log,
        TEXT("[Subsystem] SetCaptureSourceAll: %s applied to %d camera(s)."),
        NewSource == EEstuaryCaptureSource::Webcam ? TEXT("Webcam") : TEXT("Rendered Scene"),
        Applied);
}

FText UEstuarySubsystem::GetCaptureSourceLabel() const
{
    return FText::FromString(
        DesiredCaptureSource == EEstuaryCaptureSource::Webcam
            ? TEXT("Camera: Webcam")
            : TEXT("Camera: Rendered Scene"));
}

// ============================================================================
// HandleCameraCapture (Phase 7 — CAM-03)
// ============================================================================

void UEstuarySubsystem::HandleCameraCapture(const FEstuaryCameraCapture& Capture)
{
    check(IsInGameThread());

    // T-07-09: v1 routes to all registered camera components (server sends no character_id).
    // In single-character sessions this reaches exactly one component.
    int32 RoutedCount = 0;
    for (auto& KVP : RegisteredCameraComponents)
    {
        if (UEstuaryCameraComponent* Comp = KVP.Value.Get())
        {
            Comp->HandleCameraCaptureFromSubsystem(Capture);
            ++RoutedCount;
        }
    }

    UE_LOG(LogEstuary, Verbose,
        TEXT("[Subsystem] HandleCameraCapture: request_id=%s routed to %d component(s)."),
        *Capture.RequestId, RoutedCount);
}

// ============================================================================
// HandleBotAnimation (Phase 6 — ANIM-08)
// ============================================================================

void UEstuarySubsystem::HandleBotAnimation(const FEstuaryBotAnimation& Animation)
{
    check(IsInGameThread());

    // Filter terminator frames here — AnimationReceiver never sees them.
    // SDK_CONTRACT: sequence==-1, weights=={}, is_final==true marks end-of-stream.
    if (Animation.IsTerminator())
    {
        UE_LOG(LogEstuary, Verbose,
            TEXT("[Subsystem] HandleBotAnimation: terminator frame for message_id=%s — filtered."),
            *Animation.MessageId);
        return;
    }

    // bot_animation has no character_id on the wire — broadcast to all registered characters.
    // Same broadcast-to-all pattern as HandleBotVoice (each AnimationReceiver self-routes).
    for (auto& Pair : RegisteredCharacters)
    {
        if (UEstuaryCharacterComponent* Comp = Pair.Value.Get())
        {
            Comp->HandleBotAnimationFromSubsystem(Animation);
        }
    }
}

// ============================================================================
// HandleBotPose (Phase 4 — GestureSync body animation)
// ============================================================================

void UEstuarySubsystem::HandleBotPose(const FEstuaryBotPose& Pose)
{
    check(IsInGameThread());

    // Filter terminator frames here — body-animation receivers never see them.
    // SDK_CONTRACT body_animation_stream: sequence==-1, bones==[], is_final==true marks end-of-stream.
    if (Pose.IsTerminator())
    {
        UE_LOG(LogEstuary, Verbose,
            TEXT("[Subsystem] HandleBotPose: terminator frame for message_id=%s — filtered."),
            *Pose.MessageId);
        return;
    }

    // bot_pose has no character_id on the wire — broadcast to all registered characters.
    // Same broadcast-to-all pattern as HandleBotAnimation (each receiver self-routes by message_id).
    for (auto& Pair : RegisteredCharacters)
    {
        if (UEstuaryCharacterComponent* Comp = Pair.Value.Get())
        {
            Comp->HandleBotPoseFromSubsystem(Pose);
        }
    }
}

// ============================================================================
// Phase 8 — LiveKit control API implementations (LK-03, LK-04)
// ============================================================================

EEstuaryVoiceMode UEstuarySubsystem::GetVoiceMode() const
{
    return CurrentVoiceMode;
}

void UEstuarySubsystem::RequestLiveKitToken()
{
    check(IsInGameThread());

    // Project Settings → Plugins → Estuary → "Disable LiveKit (WebSocket-only voice)".
    // When set, the SDK refuses to even ask the gateway for a token, so no LiveKit
    // room is provisioned and no credits are burned. Useful during dev/debugging.
    if (const UEstuaryDeveloperSettings* Settings = UEstuaryDeveloperSettings::Get();
        Settings && Settings->bDisableLiveKit)
    {
        UE_LOG(LogEstuary, Log,
            TEXT("[Subsystem] RequestLiveKitToken — DisableLiveKit project setting is true; "
                 "staying on WebSocket voice (no token request)."));
        return;
    }

    if (SessionState != EEstuarySessionState::Connected)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Subsystem] RequestLiveKitToken — not connected, ignoring."));
        return;
    }

    if (!LiveKitBridge.IsAvailable())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Subsystem] RequestLiveKitToken — LiveKit not available "
                 "(ThirdParty/livekit-client-sdk-cpp absent or module not registered). "
                 "Remaining on WebSocket voice."));
        return;
    }

    if (LiveKitBridge.GetVoice() && LiveKitBridge.GetVoice()->IsJoined())
    {
        UE_LOG(LogEstuary, Verbose,
            TEXT("[Subsystem] RequestLiveKitToken — already joined a room. No-op."));
        return;
    }

    // Per SDK_CONTRACT §voice_livekit: emit livekit_token event with null payload.
    // Server responds asynchronously with a livekit_token event carrying {token, url, room}.
    UE_LOG(LogEstuary, Log, TEXT("[Subsystem] Emitting livekit_token request to server."));
    Bridge->EmitLiveKitTokenRequest();
}

void UEstuarySubsystem::LeaveLiveKitRoom()
{
    check(IsInGameThread());

    if (CurrentVoiceMode != EEstuaryVoiceMode::LiveKit)
    {
        // Not in LiveKit mode — nothing to tear down.
        return;
    }

    UE_LOG(LogEstuary, Log, TEXT("[Subsystem] Leaving LiveKit room."));

    if (LiveKitBridge.IsAvailable() && LiveKitBridge.GetVoice())
    {
        LiveKitBridge.GetVoice()->LeaveRoom();
        // CurrentVoiceMode is set to WebSocket inside OnLiveKitVoiceLeft which is
        // triggered by the LeaveRoom -> OnLeft delegate broadcast.
    }
    else
    {
        // Bridge unavailable but we thought we were in LiveKit mode — force reset.
        CurrentVoiceMode = EEstuaryVoiceMode::WebSocket;
    }

    // Per SDK_CONTRACT §voice_livekit Client → Server: livekit_leave (null).
    if (SessionState == EEstuarySessionState::Connected && Bridge.IsValid())
    {
        Bridge->EmitLiveKitLeave();
    }
}

// ============================================================================
// Phase 8 — LiveKit server-event handlers
// ============================================================================

void UEstuarySubsystem::HandleLiveKitTokenReceived(
    const FString& Token, const FString& Url, const FString& Room)
{
    check(IsInGameThread());

    // T-08-03: Log Url + Room only — never log the Token string (JWT credential).
    UE_LOG(LogEstuary, Log,
        TEXT("[Subsystem] livekit_token received — url=%s room=%s"), *Url, *Room);

    // Belt-and-suspenders: even if the gateway pre-bakes a token into session_info
    // (eager pre-join optimization), honor the project's WebSocket-only setting and
    // refuse to join the room. Pairs with the early-return in RequestLiveKitToken().
    if (const UEstuaryDeveloperSettings* Settings = UEstuaryDeveloperSettings::Get();
        Settings && Settings->bDisableLiveKit)
    {
        UE_LOG(LogEstuary, Log,
            TEXT("[Subsystem] livekit_token dropped — DisableLiveKit project setting is true; "
                 "staying on WebSocket voice."));
        return;
    }

    if (!LiveKitBridge.IsAvailable())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Subsystem] livekit_token received but bridge unavailable. Ignoring."));
        return;
    }

    IEstuaryLiveKitVoice* Voice = LiveKitBridge.GetVoice();
    if (!Voice)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Subsystem] livekit_token received but GetVoice() is null. Ignoring."));
        return;
    }

    // Wire the delegate callbacks before calling JoinRoom so we don't miss an
    // immediate synchronous OnJoined/OnError (unlikely but defensive).
    // AddUObject is safe here: UEstuarySubsystem is a UObject managed by the subsystem system.
    Voice->OnJoined.AddUObject(this, &UEstuarySubsystem::OnLiveKitVoiceJoined);
    Voice->OnLeft.AddUObject(this, &UEstuarySubsystem::OnLiveKitVoiceLeft);
    Voice->OnConnectionError.AddUObject(this, &UEstuarySubsystem::OnLiveKitVoiceError);

    Voice->JoinRoom(Token, Url, Room);
}

void UEstuarySubsystem::HandleLiveKitReady(const FString& Room)
{
    check(IsInGameThread());

    // Server-confirmed the bot has joined the room and Deepgram/pipeline is hot.
    // We do NOT emit livekit_join here — that already happened in OnLiveKitVoiceJoined
    // (immediately after the client-side LK SDK joined the room). Emitting it again
    // would re-trigger the server's join flow.
    UE_LOG(LogEstuary, Log,
        TEXT("[Subsystem] livekit_ready — room=%s (server bot pipeline active)"), *Room);

    bVoiceSuspended = false;
    OnVoiceReady.Broadcast(Room);
}

void UEstuarySubsystem::HandleLiveKitError(const FString& Error)
{
    check(IsInGameThread());

    UE_LOG(LogEstuary, Warning,
        TEXT("[Subsystem] livekit_error from server — %s. Remaining on WebSocket voice."), *Error);

    // Server-reported error: CurrentVoiceMode stays WebSocket (it never switched since
    // OnLiveKitVoiceJoined was not called). If the impl already attempted JoinRoom and
    // failed internally, OnLiveKitVoiceError will have already reset the mode.
}

// ============================================================================
// Phase 8 — Voice impl delegate callbacks (game-thread guaranteed by impl)
// ============================================================================

void UEstuarySubsystem::OnLiveKitVoiceJoined()
{
    check(IsInGameThread());

    UE_LOG(LogEstuary, Log,
        TEXT("[Subsystem] LiveKit room joined (client side) — switching to LiveKit voice mode."));
    CurrentVoiceMode = EEstuaryVoiceMode::LiveKit;

    // Per SDK_CONTRACT §voice_livekit: notify the gateway that the client has joined
    // the LiveKit room. This is the trigger for the server to spin up the bot
    // (LiveKit room participant + Deepgram STT pipeline). Without this emit, the
    // server never sends livekit_ready and never subscribes to our audio track —
    // STT and bot voice are silent. (Matches estuary-ts-sdk and estuary-unity-sdk
    // behavior; the previous version of this method waited for livekit_ready first
    // and deadlocked, since livekit_ready is sent only AFTER livekit_join arrives.)
    if (SessionState == EEstuarySessionState::Connected && Bridge.IsValid())
    {
        UE_LOG(LogEstuary, Log,
            TEXT("[Subsystem] Emitting livekit_join — server will spin up bot pipeline."));
        Bridge->EmitLiveKitJoin();
    }
}

void UEstuarySubsystem::OnLiveKitVoiceLeft()
{
    check(IsInGameThread());

    UE_LOG(LogEstuary, Log,
        TEXT("[Subsystem] LiveKit room left — reverting to WebSocket voice mode."));
    CurrentVoiceMode = EEstuaryVoiceMode::WebSocket;

    // Remove delegate bindings to avoid stale callbacks on the next JoinRoom.
    if (LiveKitBridge.IsAvailable() && LiveKitBridge.GetVoice())
    {
        LiveKitBridge.GetVoice()->OnJoined.RemoveAll(this);
        LiveKitBridge.GetVoice()->OnLeft.RemoveAll(this);
        LiveKitBridge.GetVoice()->OnConnectionError.RemoveAll(this);
    }

    // Belt-and-braces (stale-voice-state bug class): the room can disconnect
    // server-side WITHOUT a preceding client action or voice_timeout event
    // (e.g. the reaper deleted it, or the network dropped WebRTC). If the mic
    // is still capturing at that point it would keep pushing into a dead room
    // — or, after the mode revert above, start streaming stream_audio into a
    // session that never sent start_voice. Release local voice state; no-op on
    // the normal paths where it was already released.
    ReleaseVoiceResourcesLocal(TEXT("livekit room left"));
}

void UEstuarySubsystem::OnLiveKitVoiceError(const FString& ErrorMessage)
{
    check(IsInGameThread());

    UE_LOG(LogEstuary, Warning,
        TEXT("[Subsystem] LiveKit voice error — %s. Reverting to WebSocket voice mode."),
        *ErrorMessage);
    CurrentVoiceMode = EEstuaryVoiceMode::WebSocket;

    // Remove delegate bindings (same cleanup as OnLiveKitVoiceLeft).
    if (LiveKitBridge.IsAvailable() && LiveKitBridge.GetVoice())
    {
        LiveKitBridge.GetVoice()->OnJoined.RemoveAll(this);
        LiveKitBridge.GetVoice()->OnLeft.RemoveAll(this);
        LiveKitBridge.GetVoice()->OnConnectionError.RemoveAll(this);
    }

    // Same stale-voice-state cleanup as OnLiveKitVoiceLeft — a connection error
    // mid-call must not leave a hot mic pushing into a dead room.
    ReleaseVoiceResourcesLocal(TEXT("livekit voice error"));
}
