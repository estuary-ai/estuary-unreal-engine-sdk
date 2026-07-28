// EstuarySocketIOBridge.cpp — THE ONLY translation unit in the Estuary module that includes
// getnamo SocketIOClient-Unreal headers. No other file under Source/Estuary/ may include
// SocketIONative.h, SIOJConvert.h, or any other getnamo header. (Pitfall 21 invariant.)
//
// getnamo API targeted: SocketIOClient-Unreal v2.11.0
//   - FSocketIONative  (non-UObject; preferred over USocketIOClientComponent)
//   - OnConnectedCallback(SocketId, SessionId) / OnDisconnectedCallback(ESIOConnectionCloseReason)
//     / OnFailCallback() — all TFunction-style, all called on game thread by default
//   - OnEvent(EventName, Handler, Namespace)
//   - Connect(AddressAndPort) or Connect(FSIOConnectParams) — no namespace arg
//   - MaxReconnectionAttempts = 0  (disables internal reconnect loop — replaces v2.9 bReconnection)
//   - WebSocket-only transport is enforced via the URL scheme (wss://) — v2.11.0 has
//     no public SetTransports API; sio_client manages transport upgrade internally
//   - Emit(EventName, Payload, Callback, Namespace) — callback slot is positional;
//     pass nullptr to skip callback when only namespace is needed

#include "EstuarySocketIOBridge.h"
#include "EstuaryLog.h"

#if WITH_ESTUARY_SOCKETIO

// getnamo SocketIOClient-Unreal v2.11.0 — only in this TU.
#include "SocketIONative.h"

// UE JSON — USTRUCT <-> JSON conversion.
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "JsonObjectConverter.h"

// Threading primitives.
#include "Async/Async.h"
#include "HAL/CriticalSection.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "Misc/ScopeRWLock.h"

// ============================================================================
// File-local helpers
// ============================================================================

/** Mask an API key for safe logging: first 4 chars + "***".
 *  Threat T-02-01: api_key must never appear in plain-text logs. */
static FString MaskApiKey(const FString& Key)
{
    if (Key.Len() < 4)
    {
        return TEXT("***");
    }
    return Key.Left(4) + TEXT("***");
}

/**
 * Known top-level JSON fields for each inbound event.
 * Used to emit one-time warnings for unexpected/schema-drift fields (Pitfall 19).
 */
static const TSet<FString> KnownSessionInfoFields    = { TEXT("session_id"), TEXT("conversation_id"), TEXT("character_id"), TEXT("player_id"), TEXT("animation_capabilities") };
static const TSet<FString> KnownAuthErrorFields      = { TEXT("error") };
static const TSet<FString> KnownSessionRejectedFields = { TEXT("reason"), TEXT("cap"), TEXT("share_token_id") };
static const TSet<FString> KnownBotResponseFields    = { TEXT("text"), TEXT("is_final"), TEXT("partial"), TEXT("message_id"), TEXT("chunk_index"), TEXT("is_interjection") };
static const TSet<FString> KnownInterruptFields      = { TEXT("message_id"), TEXT("reason"), TEXT("interrupted_at") };
static const TSet<FString> KnownBotVoiceFields       = { TEXT("audio"), TEXT("message_id"), TEXT("chunk_index"), TEXT("is_final") };
static const TSet<FString> KnownSttResponseFields    = { TEXT("text"), TEXT("is_final") };
static const TSet<FString> KnownCameraCaptureFields  = { TEXT("request_id"), TEXT("text") };
static const TSet<FString> KnownErrorFields          = { TEXT("message") };
static const TSet<FString> KnownQuotaExceededFields  = { TEXT("error"), TEXT("message"), TEXT("current"), TEXT("limit"), TEXT("remaining"), TEXT("tier") };
static const TSet<FString> KnownMemoryUpdatedFields  = { TEXT("agent_id"), TEXT("player_id"), TEXT("memories_extracted"), TEXT("facts_extracted"), TEXT("conversation_id"), TEXT("new_memories"), TEXT("timestamp") };
static const TSet<FString> KnownSessionTimeoutFields = { TEXT("reason"), TEXT("idle_seconds"), TEXT("timeout_seconds") };
static const TSet<FString> KnownVoiceTimeoutFields   = { TEXT("reason"), TEXT("idle_seconds"), TEXT("timeout_seconds") };
static const TSet<FString> KnownClientActionFields   = { TEXT("name"), TEXT("arguments"), TEXT("message_id"), TEXT("chunk_index"), TEXT("timestamp") };
// MemoryData items use camelCase keys (Memory.to_dict()); sourceMessageTimestamp is
// emitted by the live server but absent from SDK_CONTRACT.md — kept here so the
// per-item schema-drift warning does not fire on it.
static const TSet<FString> KnownMemoryDataFields     = { TEXT("id"), TEXT("userId"), TEXT("agentId"), TEXT("playerId"), TEXT("content"), TEXT("memoryType"), TEXT("confidence"), TEXT("status"), TEXT("sourceConversationId"), TEXT("sourceQuote"), TEXT("source"), TEXT("topic"), TEXT("secondaryTopics"), TEXT("lastAccessedAt"), TEXT("accessCount"), TEXT("extractedAt"), TEXT("sourceMessageTimestamp"), TEXT("createdAt"), TEXT("updatedAt") };

// ============================================================================
// FImpl — pimpl definition
// ============================================================================

struct FEstuarySocketIOBridge::FImpl
{
    // getnamo native client. Allocated in Connect(), reset in Disconnect().
    TSharedPtr<FSocketIONative> Socket;

    // Cached connection params for authenticate-on-open.
    FEstuaryAuthPayload Auth;
    FString             ServerUrl;

    // Set to true in Disconnect()/destructor; checked inside every marshaled
    // dispatch so late-arriving callbacks become no-ops (Pitfall 3 defense).
    bool bShuttingDown = false;

    // Schema-drift one-shot warning cache (Pitfall 19).
    // Key format: "EventName:FieldName"
    TSet<FString>    WarnedUnknownFields;
    FCriticalSection WarnedLock;
};

// ============================================================================
// Constructor / Destructor
// ============================================================================

FEstuarySocketIOBridge::FEstuarySocketIOBridge()
{
    Impl = MakeUnique<FImpl>();
}

FEstuarySocketIOBridge::~FEstuarySocketIOBridge()
{
    if (Impl && Impl->Socket.IsValid())
    {
        Impl->bShuttingDown = true;
        // SyncDisconnect blocks until the underlying sio::client's asio thread has
        // fully drained — required to avoid the asio io_context thread firing
        // wait_handler::do_complete on a half-destroyed socket impl after Reset()
        // (see SocketIOLib sio_socket.cpp on_close access-violation crashes on PIE
        // exit when the plain async Disconnect() is used here).
        Impl->Socket->SyncDisconnect();
        Impl->Socket.Reset();
    }
}

// ============================================================================
// IsConnected
// ============================================================================

bool FEstuarySocketIOBridge::IsConnected() const
{
    if (!Impl || !Impl->Socket.IsValid())
    {
        return false;
    }
    return Impl->Socket->bIsConnected;
}

// ============================================================================
// Disconnect
// ============================================================================

void FEstuarySocketIOBridge::Disconnect()
{
    check(IsInGameThread());

    if (!Impl) { return; }

    Impl->bShuttingDown = true;  // blocks any in-flight marshaled dispatches

    if (Impl->Socket.IsValid())
    {
        UE_LOG(LogEstuary, Log, TEXT("[Bridge] Disconnect called — closing socket (sync)"));
        // SyncDisconnect (with bUnbindEventsOnDisconnect=true above) clears the
        // FSocketIONative TFunction callbacks BEFORE blocking on asio drain — so
        // no deferred handler can fire into freed lambda state.
        Impl->Socket->SyncDisconnect();
        Impl->Socket.Reset();
    }

    // Belt-and-suspenders: clear our own outbound TFunction sinks too. If a
    // GameThread AsyncTask was queued just before bShuttingDown flipped and runs
    // after SyncDisconnect returns, the early-return guard checks bShuttingDown,
    // but null sinks make a slipped-through invocation harmless.
    OnTransportConnected = nullptr;
    OnTransportClosed    = nullptr;
    OnTransportError     = nullptr;
    OnSessionInfo        = nullptr;
    OnAuthError          = nullptr;
    OnSessionRejected    = nullptr;
    OnBotResponse        = nullptr;
    OnInterruptAck       = nullptr;
    OnBotVoiceSink       = nullptr;
    OnSttResponseSink    = nullptr;
    OnCameraCaptureSink  = nullptr;
    OnVisionResponseSink = nullptr;
    OnBotAnimationSink   = nullptr;
    OnBotPoseSink        = nullptr;
    OnLiveKitTokenSink   = nullptr;
    OnLiveKitReadySink   = nullptr;
    OnLiveKitErrorSink   = nullptr;
    OnSessionTimeoutSink = nullptr;
    OnVoiceTimeoutSink   = nullptr;
    // Do NOT fire OnTransportClosed here — the subsystem initiated the disconnect
    // and already knows about it. Firing would cause a spurious Reconnecting transition.
}

// ============================================================================
// Connect
// ============================================================================

void FEstuarySocketIOBridge::Connect(const FString& ServerUrl, const FEstuaryAuthPayload& Auth)
{
    check(IsInGameThread());

    Impl->ServerUrl      = ServerUrl;
    Impl->Auth           = Auth;
    Impl->bShuttingDown  = false;
    Impl->WarnedUnknownFields.Empty();

    UE_LOG(LogEstuary, Log,
        TEXT("[Bridge] Connect → %s  char=%s player=%s rate=%d anim=%s body_anim=%s"),
        *ServerUrl,
        *Auth.CharacterId,
        *Auth.PlayerId,
        Auth.AudioSampleRate,
        Auth.bEnableAnimation ? TEXT("true") : TEXT("false"),
        Auth.bEnableBodyAnimation ? TEXT("true") : TEXT("false"));

    // -----------------------------------------------------------------------
    // Instantiate getnamo FSocketIONative
    // -----------------------------------------------------------------------
    Impl->Socket = MakeShared<FSocketIONative>();

    // Disable getnamo's internal auto-reconnect — reconnect is the subsystem's
    // responsibility with exponential backoff (Pitfall 12).
    // v2.11.0: 0 means "no retries" (replaces the v2.9-era bReconnection flag).
    Impl->Socket->MaxReconnectionAttempts = 0;

    // CRITICAL for clean PIE-end teardown: tell SyncDisconnect to clear ALL of
    // FSocketIONative's TFunction callbacks (OnConnectedCallback / OnEvent map /
    // OnDisconnectedCallback / OnFailCallback) before closing the underlying
    // sio::client. Without this, the asio io_context thread can fire deferred
    // wait_handlers that invoke our captured-WeakSelf TFunctions on freed memory
    // — observed as access violations at 0xffffffffffffffff in
    // TFunction_OwnedObject::Destroy on PIE exit.
    Impl->Socket->bUnbindEventsOnDisconnect = true;

    // Transport pinning note (Pitfall 12 / sticky sessions):
    // getnamo v2.11.0 has no public transport-selection API on FSocketIONative —
    // the underlying sio_client manages transport upgrade internally. WebSocket-only
    // is enforced by using the wss:// (or ws://) URL scheme; nginx IP-hash sticky
    // sessions remain compatible because we never long-poll.

    // -----------------------------------------------------------------------
    // Bind callbacks BEFORE Connect so no events are missed.
    // Every callback captures AsWeak() and marshals to the game thread.
    // Pattern: WeakSelf.Pin() + bShuttingDown guard on every path.
    // -----------------------------------------------------------------------

    // --- OnConnected (transport open) ---
    {
        TWeakPtr<FEstuarySocketIOBridge> WeakSelf = AsWeak();
        Impl->Socket->OnConnectedCallback = [WeakSelf](const FString& SocketId, const FString& SessionId)
        {
            AsyncTask(ENamedThreads::GameThread, [WeakSelf]()
            {
                check(IsInGameThread());
                TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelf.Pin();
                if (!Self || Self->Impl->bShuttingDown) { return; }

                // ---- Emit `authenticate` on the game thread ----
                TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

                // Build the JSON manually with snake_case keys to match SDK_CONTRACT.md.
                // FJsonObjectConverter would produce camelCase (ApiKey -> "ApiKey") which
                // does NOT match the wire contract. Manual build ensures exact key names.
                const FEstuaryAuthPayload& A = Self->Impl->Auth;
                Payload->SetStringField(TEXT("api_key"),          A.ApiKey);
                Payload->SetStringField(TEXT("character_id"),     A.CharacterId);
                Payload->SetStringField(TEXT("player_id"),        A.PlayerId);
                Payload->SetNumberField(TEXT("audio_sample_rate"), static_cast<double>(A.AudioSampleRate));
                Payload->SetBoolField  (TEXT("enable_animation"),  A.bEnableAnimation);
                Payload->SetBoolField  (TEXT("enable_body_animation"), A.bEnableBodyAnimation);
                // Diagnostic fields — server ignores unknown fields safely (Pitfall 19).
                if (!A.ClientVersion.IsEmpty())
                {
                    Payload->SetStringField(TEXT("client_version"), A.ClientVersion);
                }
                if (!A.Platform.IsEmpty())
                {
                    Payload->SetStringField(TEXT("platform"), A.Platform);
                }

                // Emit via getnamo on the /sdk namespace.
                // v2.11.0 Emit has a direct TSharedPtr<FJsonObject> overload; the previous
                // code routed through USIOJConvert::ToSIOJsonValue, but that helper only
                // accepts TArray<TSharedPtr<FJsonValue>> — not an FJsonObject — so the
                // conversion call was structurally invalid. Pass Payload straight in.
                Self->Impl->Socket->Emit(
                    TEXT("authenticate"),
                    Payload,
                    nullptr,
                    TEXT("/sdk"));

                UE_LOG(LogEstuary, Log,
                    TEXT("[Bridge] authenticate emitted char=%s player=%s rate=%d anim=%s body_anim=%s api_key=%s"),
                    *A.CharacterId, *A.PlayerId,
                    A.AudioSampleRate,
                    A.bEnableAnimation ? TEXT("true") : TEXT("false"),
                    A.bEnableBodyAnimation ? TEXT("true") : TEXT("false"),
                    *MaskApiKey(A.ApiKey));

                // Notify subsystem that transport is open.
                if (Self->OnTransportConnected)
                {
                    Self->OnTransportConnected();
                }
            });
        };
    }

    // --- OnDisconnected (transport closed) ---
    {
        TWeakPtr<FEstuarySocketIOBridge> WeakSelf = AsWeak();
        Impl->Socket->OnDisconnectedCallback = [WeakSelf](const ESIOConnectionCloseReason Reason)
        {
            // v2.11.0: only two reasons are exposed (CLOSE_REASON_NORMAL / CLOSE_REASON_DROP).
            // Map to a WS close code approximation:
            //   CLOSE_REASON_NORMAL -> 1000 (clean close)
            //   CLOSE_REASON_DROP   -> 1006 (abnormal closure / network drop)
            const int32   CloseCode = (Reason == ESIOConnectionCloseReason::CLOSE_REASON_NORMAL) ? 1000 : 1006;
            const FString ReasonStr = (Reason == ESIOConnectionCloseReason::CLOSE_REASON_NORMAL)
                                        ? TEXT("normal closure")
                                        : TEXT("connection dropped");

            AsyncTask(ENamedThreads::GameThread, [WeakSelf, CloseCode, ReasonStr]()
            {
                check(IsInGameThread());
                TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelf.Pin();
                if (!Self || Self->Impl->bShuttingDown) { return; }

                if (CloseCode != 1000)
                {
                    UE_LOG(LogEstuary, Warning,
                        TEXT("[Bridge] transport closed code=%d reason=%s"), CloseCode, *ReasonStr);
                }
                else
                {
                    UE_LOG(LogEstuary, Log,
                        TEXT("[Bridge] transport closed cleanly code=%d"), CloseCode);
                }

                if (Self->OnTransportClosed)
                {
                    Self->OnTransportClosed(CloseCode, ReasonStr);
                }
            });
        };
    }

    // --- OnFail (connection failure) ---
    {
        TWeakPtr<FEstuarySocketIOBridge> WeakSelf = AsWeak();
        Impl->Socket->OnFailCallback = [WeakSelf]()
        {
            // v2.11.0: OnFailCallback takes no arguments. Synthesize a generic message
            // so OnTransportError consumers always have a non-empty diagnostic string.
            const FString Message = TEXT("Socket.IO connection failed (no further detail from transport)");
            AsyncTask(ENamedThreads::GameThread, [WeakSelf, Message]()
            {
                check(IsInGameThread());
                TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelf.Pin();
                if (!Self || Self->Impl->bShuttingDown) { return; }

                UE_LOG(LogEstuary, Warning,
                    TEXT("[Bridge] transport error: %s"), *Message);

                if (Self->OnTransportError)
                {
                    Self->OnTransportError(Message);
                }
            });
        };
    }

    // --- Per-event handlers on /sdk namespace ---

    // Helper lambda factory that wires a getnamo event to a game-thread dispatch.
    // Defined inline so WeakSelf capture is clear.

    // session_info
    {
        TWeakPtr<FEstuarySocketIOBridge> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("session_info"),
            [WeakSelf](const FString& EventName, const TSharedPtr<FJsonValue>& EventData)
            {
                // Parse on getnamo's worker thread — safe, pure C++ struct, no UObject.
                FEstuarySessionInfo Out;
                TSharedPtr<FJsonObject> JsonObj;
                if (EventData.IsValid() && EventData->Type == EJson::Object)
                {
                    JsonObj = EventData->AsObject();
                }

                if (!JsonObj.IsValid())
                {
                    UE_LOG(LogEstuary, Warning, TEXT("[Bridge] session_info: invalid JSON payload"));
                    return;
                }

                // Map snake_case wire fields to USTRUCT members manually.
                JsonObj->TryGetStringField(TEXT("session_id"),     Out.SessionId);
                JsonObj->TryGetStringField(TEXT("conversation_id"), Out.ConversationId);
                JsonObj->TryGetStringField(TEXT("character_id"),   Out.CharacterId);
                JsonObj->TryGetStringField(TEXT("player_id"),      Out.PlayerId);

                // animation_capabilities (optional; older servers omit it).
                // SDK_CONTRACT §session_info.animation_capabilities —
                // { face: {active, provider}, body: {active, provider, rig} }.
                // body.rig identifies the wire rig ("mhr63"); the body receiver
                // hard-checks it and disables itself loudly on mismatch.
                // Unknown future modalities (e.g. gaze) are tolerated and ignored.
                const TSharedPtr<FJsonObject>* CapsObj = nullptr;
                if (JsonObj->TryGetObjectField(TEXT("animation_capabilities"), CapsObj)
                    && CapsObj != nullptr && CapsObj->IsValid())
                {
                    Out.AnimationCapabilities.bPresent = true;
                    const TSharedPtr<FJsonObject>* FaceObj = nullptr;
                    if ((*CapsObj)->TryGetObjectField(TEXT("face"), FaceObj) && FaceObj != nullptr && FaceObj->IsValid())
                    {
                        (*FaceObj)->TryGetBoolField(TEXT("active"),     Out.AnimationCapabilities.Face.bActive);
                        (*FaceObj)->TryGetStringField(TEXT("provider"), Out.AnimationCapabilities.Face.Provider);
                    }
                    const TSharedPtr<FJsonObject>* BodyObj = nullptr;
                    if ((*CapsObj)->TryGetObjectField(TEXT("body"), BodyObj) && BodyObj != nullptr && BodyObj->IsValid())
                    {
                        (*BodyObj)->TryGetBoolField(TEXT("active"),     Out.AnimationCapabilities.Body.bActive);
                        (*BodyObj)->TryGetStringField(TEXT("provider"), Out.AnimationCapabilities.Body.Provider);
                        (*BodyObj)->TryGetStringField(TEXT("rig"),      Out.AnimationCapabilities.Body.Rig);
                    }
                }

                UE_LOG(LogEstuary, Verbose,
                    TEXT("[Bridge] session_info parsed: session=%s conv=%s body_active=%d body_rig=%s face_active=%d"),
                    *Out.SessionId, *Out.ConversationId,
                    Out.AnimationCapabilities.Body.bActive ? 1 : 0,
                    Out.AnimationCapabilities.Body.Rig.IsEmpty() ? TEXT("<none>") : *Out.AnimationCapabilities.Body.Rig,
                    Out.AnimationCapabilities.Face.bActive ? 1 : 0);

                // Warn once per unknown field (Pitfall 19 schema-drift guard).
                TWeakPtr<FEstuarySocketIOBridge> WeakSelfCopy = WeakSelf;
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : JsonObj->Values)
                {
                    if (!KnownSessionInfoFields.Contains(Field.Key))
                    {
                        TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelfCopy.Pin();
                        if (Self)
                        {
                            FScopeLock Lock(&Self->Impl->WarnedLock);
                            FString CacheKey = FString::Printf(TEXT("session_info:%s"), *Field.Key);
                            if (!Self->Impl->WarnedUnknownFields.Contains(CacheKey))
                            {
                                Self->Impl->WarnedUnknownFields.Add(CacheKey);
                                UE_LOG(LogEstuary, Verbose,
                                    TEXT("[Bridge] session_info: unknown field '%s' (schema drift?)"), *Field.Key);
                            }
                        }
                    }
                }

                AsyncTask(ENamedThreads::GameThread, [WeakSelf, Out]()
                {
                    check(IsInGameThread());
                    TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelf.Pin();
                    if (!Self || Self->Impl->bShuttingDown) { return; }
                    if (Self->OnSessionInfo) { Self->OnSessionInfo(Out); }
                });
            },
            TEXT("/sdk"));
    }

    // auth_error
    {
        TWeakPtr<FEstuarySocketIOBridge> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("auth_error"),
            [WeakSelf](const FString& EventName, const TSharedPtr<FJsonValue>& EventData)
            {
                FEstuaryAuthError Out;
                TSharedPtr<FJsonObject> JsonObj;
                if (EventData.IsValid() && EventData->Type == EJson::Object)
                {
                    JsonObj = EventData->AsObject();
                }

                if (!JsonObj.IsValid())
                {
                    UE_LOG(LogEstuary, Warning, TEXT("[Bridge] auth_error: invalid JSON payload"));
                    Out.Error = TEXT("invalid_payload");
                }
                else
                {
                    JsonObj->TryGetStringField(TEXT("error"), Out.Error);

                    TWeakPtr<FEstuarySocketIOBridge> WeakSelfCopy = WeakSelf;
                    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : JsonObj->Values)
                    {
                        if (!KnownAuthErrorFields.Contains(Field.Key))
                        {
                            TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelfCopy.Pin();
                            if (Self)
                            {
                                FScopeLock Lock(&Self->Impl->WarnedLock);
                                FString CacheKey = FString::Printf(TEXT("auth_error:%s"), *Field.Key);
                                if (!Self->Impl->WarnedUnknownFields.Contains(CacheKey))
                                {
                                    Self->Impl->WarnedUnknownFields.Add(CacheKey);
                                    UE_LOG(LogEstuary, Verbose,
                                        TEXT("[Bridge] auth_error: unknown field '%s'"), *Field.Key);
                                }
                            }
                        }
                    }
                }

                UE_LOG(LogEstuary, Warning, TEXT("[Bridge] auth_error received: %s"), *Out.Error);

                AsyncTask(ENamedThreads::GameThread, [WeakSelf, Out]()
                {
                    check(IsInGameThread());
                    TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelf.Pin();
                    if (!Self || Self->Impl->bShuttingDown) { return; }
                    if (Self->OnAuthError) { Self->OnAuthError(Out); }
                });
            },
            TEXT("/sdk"));
    }

    // session_rejected
    {
        TWeakPtr<FEstuarySocketIOBridge> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("session_rejected"),
            [WeakSelf](const FString& EventName, const TSharedPtr<FJsonValue>& EventData)
            {
                FEstuarySessionRejected Out;
                TSharedPtr<FJsonObject> JsonObj;
                if (EventData.IsValid() && EventData->Type == EJson::Object)
                {
                    JsonObj = EventData->AsObject();
                }

                if (!JsonObj.IsValid())
                {
                    UE_LOG(LogEstuary, Warning, TEXT("[Bridge] session_rejected: invalid JSON payload"));
                    Out.Reason = TEXT("unknown");
                }
                else
                {
                    JsonObj->TryGetStringField(TEXT("reason"),         Out.Reason);
                    JsonObj->TryGetStringField(TEXT("share_token_id"), Out.ShareTokenId);

                    int32 CapVal = 0;
                    if (JsonObj->TryGetNumberField(TEXT("cap"), CapVal))
                    {
                        Out.Cap = CapVal;
                    }

                    TWeakPtr<FEstuarySocketIOBridge> WeakSelfCopy = WeakSelf;
                    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : JsonObj->Values)
                    {
                        if (!KnownSessionRejectedFields.Contains(Field.Key))
                        {
                            TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelfCopy.Pin();
                            if (Self)
                            {
                                FScopeLock Lock(&Self->Impl->WarnedLock);
                                FString CacheKey = FString::Printf(TEXT("session_rejected:%s"), *Field.Key);
                                if (!Self->Impl->WarnedUnknownFields.Contains(CacheKey))
                                {
                                    Self->Impl->WarnedUnknownFields.Add(CacheKey);
                                    UE_LOG(LogEstuary, Verbose,
                                        TEXT("[Bridge] session_rejected: unknown field '%s'"), *Field.Key);
                                }
                            }
                        }
                    }
                }

                UE_LOG(LogEstuary, Warning,
                    TEXT("[Bridge] session_rejected: reason=%s cap=%d"), *Out.Reason, Out.Cap);

                AsyncTask(ENamedThreads::GameThread, [WeakSelf, Out]()
                {
                    check(IsInGameThread());
                    TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelf.Pin();
                    if (!Self || Self->Impl->bShuttingDown) { return; }
                    if (Self->OnSessionRejected) { Self->OnSessionRejected(Out); }
                });
            },
            TEXT("/sdk"));
    }

    // error — non-terminal server error (SDK_CONTRACT §error: {message}).
    // Clone of the auth_error path; session is NOT torn down on this event.
    {
        TWeakPtr<FEstuarySocketIOBridge> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("error"),
            [WeakSelf](const FString& EventName, const TSharedPtr<FJsonValue>& EventData)
            {
                FEstuaryError Out;
                TSharedPtr<FJsonObject> JsonObj;
                if (EventData.IsValid() && EventData->Type == EJson::Object)
                {
                    JsonObj = EventData->AsObject();
                }

                if (!JsonObj.IsValid())
                {
                    UE_LOG(LogEstuary, Warning, TEXT("[Bridge] error: invalid JSON payload"));
                }
                else
                {
                    JsonObj->TryGetStringField(TEXT("message"), Out.Message);

                    TWeakPtr<FEstuarySocketIOBridge> WeakSelfCopy = WeakSelf;
                    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : JsonObj->Values)
                    {
                        if (!KnownErrorFields.Contains(Field.Key))
                        {
                            TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelfCopy.Pin();
                            if (Self)
                            {
                                FScopeLock Lock(&Self->Impl->WarnedLock);
                                FString CacheKey = FString::Printf(TEXT("error:%s"), *Field.Key);
                                if (!Self->Impl->WarnedUnknownFields.Contains(CacheKey))
                                {
                                    Self->Impl->WarnedUnknownFields.Add(CacheKey);
                                    UE_LOG(LogEstuary, Verbose,
                                        TEXT("[Bridge] error: unknown field '%s'"), *Field.Key);
                                }
                            }
                        }
                    }
                }

                UE_LOG(LogEstuary, Warning, TEXT("[Bridge] error received: %s"), *Out.Message);

                AsyncTask(ENamedThreads::GameThread, [WeakSelf, Out]()
                {
                    check(IsInGameThread());
                    TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelf.Pin();
                    if (!Self || Self->Impl->bShuttingDown) { return; }
                    if (Self->OnError) { Self->OnError(Out); }
                });
            },
            TEXT("/sdk"));
    }

    // quota_exceeded — quota / share-interaction-limit (SDK_CONTRACT §quota_exceeded).
    // Clone of the auth_error path. The share-limit variant OMITS the numeric fields
    // (current/limit/remaining/tier); TryGetNumberField leaves the struct defaults (0)
    // untouched, so missing fields are tolerated. The gateway disconnects immediately
    // after — no further events follow.
    {
        TWeakPtr<FEstuarySocketIOBridge> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("quota_exceeded"),
            [WeakSelf](const FString& EventName, const TSharedPtr<FJsonValue>& EventData)
            {
                FEstuaryQuotaExceeded Out;
                TSharedPtr<FJsonObject> JsonObj;
                if (EventData.IsValid() && EventData->Type == EJson::Object)
                {
                    JsonObj = EventData->AsObject();
                }

                if (!JsonObj.IsValid())
                {
                    UE_LOG(LogEstuary, Warning, TEXT("[Bridge] quota_exceeded: invalid JSON payload"));
                    Out.ErrorCode = TEXT("quota_exceeded");
                }
                else
                {
                    JsonObj->TryGetStringField(TEXT("error"),   Out.ErrorCode);
                    JsonObj->TryGetStringField(TEXT("message"), Out.Message);
                    JsonObj->TryGetStringField(TEXT("tier"),    Out.Tier);

                    // Numeric fields are OPTIONAL (omitted in the share-limit variant).
                    int32 IntVal = 0;
                    if (JsonObj->TryGetNumberField(TEXT("current"),   IntVal)) { Out.Current   = IntVal; }
                    if (JsonObj->TryGetNumberField(TEXT("limit"),     IntVal)) { Out.Limit     = IntVal; }
                    if (JsonObj->TryGetNumberField(TEXT("remaining"), IntVal)) { Out.Remaining = IntVal; }

                    TWeakPtr<FEstuarySocketIOBridge> WeakSelfCopy = WeakSelf;
                    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : JsonObj->Values)
                    {
                        if (!KnownQuotaExceededFields.Contains(Field.Key))
                        {
                            TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelfCopy.Pin();
                            if (Self)
                            {
                                FScopeLock Lock(&Self->Impl->WarnedLock);
                                FString CacheKey = FString::Printf(TEXT("quota_exceeded:%s"), *Field.Key);
                                if (!Self->Impl->WarnedUnknownFields.Contains(CacheKey))
                                {
                                    Self->Impl->WarnedUnknownFields.Add(CacheKey);
                                    UE_LOG(LogEstuary, Verbose,
                                        TEXT("[Bridge] quota_exceeded: unknown field '%s'"), *Field.Key);
                                }
                            }
                        }
                    }
                }

                UE_LOG(LogEstuary, Warning,
                    TEXT("[Bridge] quota_exceeded received: error=%s current=%d limit=%d remaining=%d tier=%s"),
                    *Out.ErrorCode, Out.Current, Out.Limit, Out.Remaining, *Out.Tier);

                AsyncTask(ENamedThreads::GameThread, [WeakSelf, Out]()
                {
                    check(IsInGameThread());
                    TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelf.Pin();
                    if (!Self || Self->Impl->bShuttingDown) { return; }
                    if (Self->OnQuotaExceeded) { Self->OnQuotaExceeded(Out); }
                });
            },
            TEXT("/sdk"));
    }

    // memory_updated — newly extracted memories pushed after a conversation ends
    // (SDK_CONTRACT §memory_push / MemoryUpdatedEvent). Top-level keys are snake_case;
    // new_memories[] items use camelCase keys (Memory.to_dict()). Array-of-objects parse
    // mirrors the bot_pose `bones` loop.
    {
        TWeakPtr<FEstuarySocketIOBridge> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("memory_updated"),
            [WeakSelf](const FString& EventName, const TSharedPtr<FJsonValue>& EventData)
            {
                FEstuaryMemoryUpdated Out;
                TSharedPtr<FJsonObject> JsonObj;
                if (EventData.IsValid() && EventData->Type == EJson::Object)
                {
                    JsonObj = EventData->AsObject();
                }

                if (!JsonObj.IsValid())
                {
                    UE_LOG(LogEstuary, Warning, TEXT("[Bridge] memory_updated: invalid JSON payload"));
                }
                else
                {
                    // Top-level snake_case fields.
                    JsonObj->TryGetStringField(TEXT("agent_id"),        Out.AgentId);
                    JsonObj->TryGetStringField(TEXT("player_id"),       Out.PlayerId);
                    JsonObj->TryGetStringField(TEXT("conversation_id"), Out.ConversationId);
                    JsonObj->TryGetStringField(TEXT("timestamp"),       Out.Timestamp);
                    {
                        int32 IntVal = 0;
                        if (JsonObj->TryGetNumberField(TEXT("memories_extracted"), IntVal)) { Out.MemoriesExtracted = IntVal; }
                        if (JsonObj->TryGetNumberField(TEXT("facts_extracted"),    IntVal)) { Out.FactsExtracted    = IntVal; }
                    }

                    // new_memories[]: array of objects, each with camelCase keys.
                    const TArray<TSharedPtr<FJsonValue>>* MemArr = nullptr;
                    if (JsonObj->TryGetArrayField(TEXT("new_memories"), MemArr) && MemArr)
                    {
                        Out.NewMemories.Reserve(MemArr->Num());
                        for (const TSharedPtr<FJsonValue>& MemVal : *MemArr)
                        {
                            if (!MemVal.IsValid() || MemVal->Type != EJson::Object)
                            {
                                UE_LOG(LogEstuary, Verbose,
                                    TEXT("[Bridge] memory_updated: skipping non-object new_memories entry."));
                                continue;
                            }
                            const TSharedPtr<FJsonObject> MemObj = MemVal->AsObject();
                            if (!MemObj.IsValid()) { continue; }

                            FEstuaryMemoryData Mem;
                            // camelCase wire keys (Memory.to_dict()).
                            MemObj->TryGetStringField(TEXT("id"),          Mem.Id);
                            MemObj->TryGetStringField(TEXT("content"),     Mem.Content);
                            MemObj->TryGetStringField(TEXT("memoryType"),  Mem.MemoryType);
                            MemObj->TryGetStringField(TEXT("status"),      Mem.Status);
                            MemObj->TryGetStringField(TEXT("topic"),       Mem.Topic);
                            MemObj->TryGetStringField(TEXT("sourceQuote"), Mem.SourceQuote);
                            MemObj->TryGetStringField(TEXT("createdAt"),   Mem.CreatedAt);
                            MemObj->TryGetStringField(TEXT("updatedAt"),   Mem.UpdatedAt);
                            {
                                double NumVal = 0.0;
                                if (MemObj->TryGetNumberField(TEXT("confidence"), NumVal))
                                {
                                    Mem.Confidence = static_cast<float>(NumVal);
                                }
                                int32 IntVal = 0;
                                if (MemObj->TryGetNumberField(TEXT("accessCount"), IntVal))
                                {
                                    Mem.AccessCount = IntVal;
                                }
                            }

                            // secondaryTopics: array of strings.
                            const TArray<TSharedPtr<FJsonValue>>* TopicsArr = nullptr;
                            if (MemObj->TryGetArrayField(TEXT("secondaryTopics"), TopicsArr) && TopicsArr)
                            {
                                Mem.SecondaryTopics.Reserve(TopicsArr->Num());
                                for (const TSharedPtr<FJsonValue>& TopicVal : *TopicsArr)
                                {
                                    FString TopicStr;
                                    if (TopicVal.IsValid() && TopicVal->TryGetString(TopicStr))
                                    {
                                        Mem.SecondaryTopics.Add(MoveTemp(TopicStr));
                                    }
                                }
                            }

                            // Per-item schema-drift warning (Pitfall 19) on the camelCase keys.
                            TWeakPtr<FEstuarySocketIOBridge> WeakSelfItem = WeakSelf;
                            for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : MemObj->Values)
                            {
                                if (!KnownMemoryDataFields.Contains(Field.Key))
                                {
                                    TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelfItem.Pin();
                                    if (Self)
                                    {
                                        FScopeLock Lock(&Self->Impl->WarnedLock);
                                        FString CacheKey = FString::Printf(TEXT("memory_data:%s"), *Field.Key);
                                        if (!Self->Impl->WarnedUnknownFields.Contains(CacheKey))
                                        {
                                            Self->Impl->WarnedUnknownFields.Add(CacheKey);
                                            UE_LOG(LogEstuary, Verbose,
                                                TEXT("[Bridge] memory_updated: unknown new_memories field '%s'"), *Field.Key);
                                        }
                                    }
                                }
                            }

                            Out.NewMemories.Add(MoveTemp(Mem));
                        }
                    }

                    // Top-level schema-drift warning.
                    TWeakPtr<FEstuarySocketIOBridge> WeakSelfCopy = WeakSelf;
                    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : JsonObj->Values)
                    {
                        if (!KnownMemoryUpdatedFields.Contains(Field.Key))
                        {
                            TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelfCopy.Pin();
                            if (Self)
                            {
                                FScopeLock Lock(&Self->Impl->WarnedLock);
                                FString CacheKey = FString::Printf(TEXT("memory_updated:%s"), *Field.Key);
                                if (!Self->Impl->WarnedUnknownFields.Contains(CacheKey))
                                {
                                    Self->Impl->WarnedUnknownFields.Add(CacheKey);
                                    UE_LOG(LogEstuary, Verbose,
                                        TEXT("[Bridge] memory_updated: unknown field '%s'"), *Field.Key);
                                }
                            }
                        }
                    }
                }

                UE_LOG(LogEstuary, Log,
                    TEXT("[Bridge] memory_updated received: agent=%s player=%s extracted=%d new=%d"),
                    *Out.AgentId, *Out.PlayerId, Out.MemoriesExtracted, Out.NewMemories.Num());

                AsyncTask(ENamedThreads::GameThread, [WeakSelf, Out]()
                {
                    check(IsInGameThread());
                    TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelf.Pin();
                    if (!Self || Self->Impl->bShuttingDown) { return; }
                    if (Self->OnMemoryUpdated) { Self->OnMemoryUpdated(Out); }
                });
            },
            TEXT("/sdk"));
    }

    // client_action — typed in-world action calls via native LLM function calling
    // (SDK_CONTRACT §client_action, contract v1.9 / SCRUM-202). Exactly one event
    // per action call; fire-on-arrival. `arguments` values arrive as
    // string|number|boolean JSON (validated server-side against the character's
    // declared parameters) and are stringified into the Blueprint-friendly
    // TMap<FString,FString>: numbers invariant ("2.5", integral values without a
    // decimal point), booleans lowercase "true"/"false" — the same convention as
    // the Unity / Lens SDKs.
    {
        TWeakPtr<FEstuarySocketIOBridge> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("client_action"),
            [WeakSelf](const FString& EventName, const TSharedPtr<FJsonValue>& EventData)
            {
                FEstuaryClientAction Out;
                TSharedPtr<FJsonObject> JsonObj;
                if (EventData.IsValid() && EventData->Type == EJson::Object)
                {
                    JsonObj = EventData->AsObject();
                }

                if (!JsonObj.IsValid())
                {
                    UE_LOG(LogEstuary, Warning, TEXT("[Bridge] client_action: invalid JSON payload"));
                }
                else
                {
                    JsonObj->TryGetStringField(TEXT("name"),       Out.Name);
                    JsonObj->TryGetStringField(TEXT("message_id"), Out.MessageId);
                    JsonObj->TryGetStringField(TEXT("timestamp"),  Out.Timestamp);
                    {
                        int32 IntVal = 0;
                        if (JsonObj->TryGetNumberField(TEXT("chunk_index"), IntVal)) { Out.ChunkIndex = IntVal; }
                    }

                    // arguments{}: dynamic keys, scalar string|number|boolean values.
                    // TPair<FString, ...> materializes an FString key — required for
                    // UE 5.8 where FJsonObject keys are UE::FSharedString (see the
                    // file's other Values loops).
                    const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
                    if (JsonObj->TryGetObjectField(TEXT("arguments"), ArgsObj) && ArgsObj && ArgsObj->IsValid())
                    {
                        Out.Arguments.Reserve((*ArgsObj)->Values.Num());
                        for (const TPair<FString, TSharedPtr<FJsonValue>>& Arg : (*ArgsObj)->Values)
                        {
                            if (!Arg.Value.IsValid()) { continue; }

                            FString ValueStr;
                            switch (Arg.Value->Type)
                            {
                            case EJson::String:
                                ValueStr = Arg.Value->AsString();
                                break;
                            case EJson::Boolean:
                                ValueStr = Arg.Value->AsBool() ? TEXT("true") : TEXT("false");
                                break;
                            case EJson::Number:
                            {
                                const double Num = Arg.Value->AsNumber();
                                const int64 AsInt = (Num > -9.0e18 && Num < 9.0e18)
                                    ? static_cast<int64>(Num) : 0;
                                if (static_cast<double>(AsInt) == Num)
                                {
                                    // Integral values print without a decimal point
                                    // ("3", not "3.0") to match Unity/Lens stringification.
                                    ValueStr = FString::Printf(TEXT("%lld"), AsInt);
                                }
                                else
                                {
                                    ValueStr = FString::SanitizeFloat(Num, 0);
                                }
                                break;
                            }
                            default:
                                // Server coerces arguments to string/number/boolean —
                                // anything else is schema drift; skip defensively.
                                UE_LOG(LogEstuary, Verbose,
                                    TEXT("[Bridge] client_action: skipping non-scalar argument '%s'"), *Arg.Key);
                                continue;
                            }

                            Out.Arguments.Add(Arg.Key, MoveTemp(ValueStr));
                        }
                    }

                    // Top-level schema-drift warning (Pitfall 19).
                    TWeakPtr<FEstuarySocketIOBridge> WeakSelfCopy = WeakSelf;
                    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : JsonObj->Values)
                    {
                        if (!KnownClientActionFields.Contains(Field.Key))
                        {
                            TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelfCopy.Pin();
                            if (Self)
                            {
                                FScopeLock Lock(&Self->Impl->WarnedLock);
                                FString CacheKey = FString::Printf(TEXT("client_action:%s"), *Field.Key);
                                if (!Self->Impl->WarnedUnknownFields.Contains(CacheKey))
                                {
                                    Self->Impl->WarnedUnknownFields.Add(CacheKey);
                                    UE_LOG(LogEstuary, Verbose,
                                        TEXT("[Bridge] client_action: unknown field '%s' (schema drift?)"), *Field.Key);
                                }
                            }
                        }
                    }
                }

                UE_LOG(LogEstuary, Log,
                    TEXT("[Bridge] client_action received: name=%s args=%d message_id=%s chunk_index=%d"),
                    *Out.Name, Out.Arguments.Num(), *Out.MessageId, Out.ChunkIndex);

                AsyncTask(ENamedThreads::GameThread, [WeakSelf, Out]()
                {
                    check(IsInGameThread());
                    TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelf.Pin();
                    if (!Self || Self->Impl->bShuttingDown) { return; }
                    if (Self->OnClientAction) { Self->OnClientAction(Out); }
                });
            },
            TEXT("/sdk"));
    }

    // bot_response — streaming chunks + final chunk.
    {
        TWeakPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("bot_response"),
            [WeakSelf](const FString& /*EventName*/, const TSharedPtr<FJsonValue>& Value)
            {
                if (!Value.IsValid() || Value->Type != EJson::Object)
                {
                    UE_LOG(LogEstuary, Warning, TEXT("[Bridge] bot_response: non-object payload"));
                    return;
                }
                const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
                if (!Obj.IsValid()) { return; }

                // Parse defensively — every field optional; Pitfall 19 zero-safe defaults apply.
                FEstuaryBotResponse Parsed;
                Obj->TryGetStringField(TEXT("text"),            Parsed.Text);
                Obj->TryGetStringField(TEXT("partial"),         Parsed.Partial);
                Obj->TryGetStringField(TEXT("message_id"),      Parsed.MessageId);
                Obj->TryGetBoolField  (TEXT("is_final"),        Parsed.bIsFinal);
                Obj->TryGetBoolField  (TEXT("is_interjection"),  Parsed.bIsInterjection);
                {
                    double ChunkIdxD = -1.0;
                    if (Obj->TryGetNumberField(TEXT("chunk_index"), ChunkIdxD))
                    {
                        Parsed.ChunkIndex = static_cast<int32>(ChunkIdxD);
                    }
                }
                // CharacterId is subsystem-populated on dispatch; leave empty here.

                // Schema-drift warning (Pitfall 19): log unknown top-level fields ONCE per key.
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : Obj->Values)
                {
                    if (!KnownBotResponseFields.Contains(Kv.Key))
                    {
                        const FString WarnKey = FString::Printf(TEXT("bot_response:%s"), *Kv.Key);
                        if (auto Pinned = WeakSelf.Pin())
                        {
                            FScopeLock Lock(&Pinned->Impl->WarnedLock);
                            if (!Pinned->Impl->WarnedUnknownFields.Contains(WarnKey))
                            {
                                Pinned->Impl->WarnedUnknownFields.Add(WarnKey);
                                UE_LOG(LogEstuary, Verbose,
                                    TEXT("[Bridge] schema-drift: bot_response has unknown field '%s'"),
                                    *Kv.Key);
                            }
                        }
                    }
                }

                // Marshal to game thread; lifetime guard via TSharedFromThis weak ptr.
                AsyncTask(ENamedThreads::GameThread,
                    [WeakSelf, Parsed]()
                    {
                        check(IsInGameThread());
                        TSharedPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin();
                        if (!Pinned.IsValid() || !Pinned->Impl.IsValid() || Pinned->Impl->bShuttingDown) { return; }
                        if (Pinned->OnBotResponse)
                        {
                            Pinned->OnBotResponse(Parsed);
                        }
                    });
            },
            TEXT("/sdk"));
    }

    // interrupt — server ack or server-initiated interrupt.
    {
        TWeakPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("interrupt"),
            [WeakSelf](const FString& /*EventName*/, const TSharedPtr<FJsonValue>& Value)
            {
                FEstuaryInterruptPayload Parsed;

                if (Value.IsValid() && Value->Type == EJson::Object)
                {
                    const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
                    if (Obj.IsValid())
                    {
                        Obj->TryGetStringField(TEXT("message_id"),     Parsed.MessageId);
                        Obj->TryGetStringField(TEXT("reason"),         Parsed.Reason);
                        Obj->TryGetStringField(TEXT("interrupted_at"), Parsed.InterruptedAt);

                        // Schema-drift logging.
                        for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : Obj->Values)
                        {
                            if (!KnownInterruptFields.Contains(Kv.Key))
                            {
                                const FString WarnKey = FString::Printf(TEXT("interrupt:%s"), *Kv.Key);
                                if (auto Pinned = WeakSelf.Pin())
                                {
                                    FScopeLock Lock(&Pinned->Impl->WarnedLock);
                                    if (!Pinned->Impl->WarnedUnknownFields.Contains(WarnKey))
                                    {
                                        Pinned->Impl->WarnedUnknownFields.Add(WarnKey);
                                        UE_LOG(LogEstuary, Verbose,
                                            TEXT("[Bridge] schema-drift: interrupt has unknown field '%s'"),
                                            *Kv.Key);
                                    }
                                }
                            }
                        }
                    }
                }
                // else: empty/null payload is allowed (contract says every field optional).

                AsyncTask(ENamedThreads::GameThread,
                    [WeakSelf, Parsed]()
                    {
                        check(IsInGameThread());
                        TSharedPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin();
                        if (!Pinned.IsValid() || !Pinned->Impl.IsValid() || Pinned->Impl->bShuttingDown) { return; }
                        if (Pinned->OnInterruptAck)
                        {
                            Pinned->OnInterruptAck(Parsed);
                        }
                    });
            },
            TEXT("/sdk"));
    }

    // bot_voice — streaming TTS audio chunks (AUDIO-01, AUDIO-03).
    {
        TWeakPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("bot_voice"),
            [WeakSelf](const FString& /*EventName*/, const TSharedPtr<FJsonValue>& Value)
            {
                // Parse on getnamo's worker thread — safe: pure C++ struct, no UObject.
                // T-04-01-01 mitigation: use default sentinel values for all missing fields;
                // never crash on partial JSON (Pitfall 19 zero-safe defaults).
                FEstuaryBotVoice Parsed;

                if (!Value.IsValid() || Value->Type != EJson::Object)
                {
                    // T-04-01-01: malformed payload — use safe defaults, do not crash.
                    // Fire sink with empty Voice so consumer sees bIsFinal=false, Audio="".
                    AsyncTask(ENamedThreads::GameThread,
                        [WeakSelf, Parsed]()
                        {
                            check(IsInGameThread());
                            TSharedPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin();
                            if (!Pinned.IsValid() || !Pinned->Impl.IsValid() || Pinned->Impl->bShuttingDown) { return; }
                            if (Pinned->OnBotVoiceSink)
                            {
                                Pinned->OnBotVoiceSink(Parsed);
                            }
                        });
                    return;
                }

                const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
                if (!Obj.IsValid()) { return; }

                // Parse with exact SDK_CONTRACT field names (snake_case).
                Obj->TryGetStringField(TEXT("audio"),      Parsed.Audio);
                Obj->TryGetStringField(TEXT("message_id"), Parsed.MessageId);
                Obj->TryGetBoolField  (TEXT("is_final"),   Parsed.bIsFinal);
                {
                    double ChunkIdxD = -1.0;
                    if (Obj->TryGetNumberField(TEXT("chunk_index"), ChunkIdxD))
                    {
                        Parsed.ChunkIndex = static_cast<int32>(ChunkIdxD);
                    }
                    // else: ChunkIndex stays at default -1 sentinel
                }

                // Schema-drift warning (Pitfall 19): log unknown top-level fields ONCE per key.
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : Obj->Values)
                {
                    if (!KnownBotVoiceFields.Contains(Kv.Key))
                    {
                        const FString WarnKey = FString::Printf(TEXT("bot_voice:%s"), *Kv.Key);
                        if (auto Pinned = WeakSelf.Pin())
                        {
                            FScopeLock Lock(&Pinned->Impl->WarnedLock);
                            if (!Pinned->Impl->WarnedUnknownFields.Contains(WarnKey))
                            {
                                Pinned->Impl->WarnedUnknownFields.Add(WarnKey);
                                UE_LOG(LogEstuary, Verbose,
                                    TEXT("[Bridge] schema-drift: bot_voice has unknown field '%s'"),
                                    *Kv.Key);
                            }
                        }
                    }
                }

                // Marshal to game thread — same pattern as bot_response (Pitfall 1).
                AsyncTask(ENamedThreads::GameThread,
                    [WeakSelf, Parsed]()
                    {
                        check(IsInGameThread());
                        TSharedPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin();
                        if (!Pinned.IsValid() || !Pinned->Impl.IsValid() || Pinned->Impl->bShuttingDown) { return; }
                        if (Pinned->OnBotVoiceSink)
                        {
                            Pinned->OnBotVoiceSink(Parsed);
                        }
                    });
            },
            TEXT("/sdk"));
    }

    // stt_response — server-to-client STT transcript (Phase 5; MIC-04).
    {
        TWeakPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("stt_response"),
            [WeakSelf](const FString& /*EventName*/, const TSharedPtr<FJsonValue>& Value)
            {
                // Parse on getnamo's worker thread — safe: pure C++ struct, no UObject.
                // T-05-01-A mitigation: text field is display-only; no eval path.
                FEstuarySttResponse Parsed;

                if (!Value.IsValid() || Value->Type != EJson::Object)
                {
                    UE_LOG(LogEstuary, Warning, TEXT("[Bridge] stt_response: non-object payload — using safe defaults"));
                    AsyncTask(ENamedThreads::GameThread,
                        [WeakSelf, Parsed]()
                        {
                            check(IsInGameThread());
                            TSharedPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin();
                            if (!Pinned.IsValid() || !Pinned->Impl.IsValid() || Pinned->Impl->bShuttingDown) { return; }
                            if (Pinned->OnSttResponseSink) { Pinned->OnSttResponseSink(Parsed); }
                        });
                    return;
                }

                const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
                if (!Obj.IsValid()) { return; }

                // Parse with exact SDK_CONTRACT field names.
                Obj->TryGetStringField(TEXT("text"),     Parsed.Text);
                Obj->TryGetBoolField  (TEXT("is_final"), Parsed.bIsFinal);
                // CharacterId is NOT on the wire — subsystem populates it on dispatch.

                // T-05-01-C mitigation: do NOT log the text field value.
                UE_LOG(LogEstuary, Verbose,
                    TEXT("[Bridge] stt_response parsed: is_final=%d text_len=%d"),
                    Parsed.bIsFinal ? 1 : 0, Parsed.Text.Len());

                // Schema-drift warning (Pitfall 19).
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : Obj->Values)
                {
                    if (!KnownSttResponseFields.Contains(Kv.Key))
                    {
                        const FString WarnKey = FString::Printf(TEXT("stt_response:%s"), *Kv.Key);
                        if (auto Pinned = WeakSelf.Pin())
                        {
                            FScopeLock Lock(&Pinned->Impl->WarnedLock);
                            if (!Pinned->Impl->WarnedUnknownFields.Contains(WarnKey))
                            {
                                Pinned->Impl->WarnedUnknownFields.Add(WarnKey);
                                UE_LOG(LogEstuary, Verbose,
                                    TEXT("[Bridge] schema-drift: stt_response has unknown field '%s'"),
                                    *Kv.Key);
                            }
                        }
                    }
                }

                // Marshal to game thread — identical pattern to bot_response (Pitfall 1).
                AsyncTask(ENamedThreads::GameThread,
                    [WeakSelf, Parsed]()
                    {
                        check(IsInGameThread());
                        TSharedPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin();
                        if (!Pinned.IsValid() || !Pinned->Impl.IsValid() || Pinned->Impl->bShuttingDown) { return; }
                        if (Pinned->OnSttResponseSink)
                        {
                            Pinned->OnSttResponseSink(Parsed);
                        }
                    });
            },
            TEXT("/sdk"));
    }

    // camera_capture — server-initiated capture request (Phase 7; CAM-03).
    // T-07-01: Null-check all parsed fields; empty defaults for missing optional keys;
    // never pass raw JSON pointers across the marshal boundary (Pitfall 3).
    {
        TWeakPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("camera_capture"),
            [WeakSelf](const FString& /*EventName*/, const TSharedPtr<FJsonValue>& Value)
            {
                // Parse on getnamo's worker thread — safe: pure C++ struct, no UObject.
                // T-07-01 mitigation: use empty-string defaults for ALL fields.
                FEstuaryCameraCapture Parsed;

                if (!Value.IsValid() || Value->Type != EJson::Object)
                {
                    UE_LOG(LogEstuary, Warning,
                        TEXT("[Bridge] camera_capture: non-object payload — using safe defaults"));
                    AsyncTask(ENamedThreads::GameThread,
                        [WeakSelf, Parsed]()
                        {
                            check(IsInGameThread());
                            TSharedPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin();
                            if (!Pinned.IsValid() || !Pinned->Impl.IsValid() || Pinned->Impl->bShuttingDown) { return; }
                            if (Pinned->OnCameraCaptureSink) { Pinned->OnCameraCaptureSink(Parsed); }
                        });
                    return;
                }

                const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
                if (!Obj.IsValid()) { return; }

                // Parse with exact SDK_CONTRACT field names (snake_case).
                // T-07-01: TryGetStringField returns false and leaves out unchanged if field absent.
                Obj->TryGetStringField(TEXT("request_id"), Parsed.RequestId);
                Obj->TryGetStringField(TEXT("text"),       Parsed.Text);

                UE_LOG(LogEstuary, Verbose,
                    TEXT("[Bridge] camera_capture parsed: request_id=%s has_text=%d"),
                    *Parsed.RequestId, Parsed.Text.Len() > 0 ? 1 : 0);

                // Schema-drift warning (Pitfall 19).
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : Obj->Values)
                {
                    if (!KnownCameraCaptureFields.Contains(Kv.Key))
                    {
                        const FString WarnKey = FString::Printf(TEXT("camera_capture:%s"), *Kv.Key);
                        if (auto Pinned = WeakSelf.Pin())
                        {
                            FScopeLock Lock(&Pinned->Impl->WarnedLock);
                            if (!Pinned->Impl->WarnedUnknownFields.Contains(WarnKey))
                            {
                                Pinned->Impl->WarnedUnknownFields.Add(WarnKey);
                                UE_LOG(LogEstuary, Verbose,
                                    TEXT("[Bridge] schema-drift: camera_capture has unknown field '%s'"),
                                    *Kv.Key);
                            }
                        }
                    }
                }

                // Marshal to game thread — same pattern as bot_response (Pitfall 1).
                AsyncTask(ENamedThreads::GameThread,
                    [WeakSelf, Parsed]()
                    {
                        check(IsInGameThread());
                        TSharedPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin();
                        if (!Pinned.IsValid() || !Pinned->Impl.IsValid() || Pinned->Impl->bShuttingDown) { return; }
                        if (Pinned->OnCameraCaptureSink)
                        {
                            Pinned->OnCameraCaptureSink(Parsed);
                        }
                    });
            },
            TEXT("/sdk"));
    }

    // bot_animation — ARKit-52 blendshape frames (Phase 6; ANIM-08).
    // No schema-drift set needed here; terminator filtering is done in subsystem.
    {
        TWeakPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("bot_animation"),
            [WeakSelf](const FString& /*EventName*/, const TSharedPtr<FJsonValue>& Value)
            {
                // Parse on getnamo's worker thread — safe: pure C++ struct, no UObject.
                FEstuaryBotAnimation Parsed;

                if (!Value.IsValid() || Value->Type != EJson::Object)
                {
                    UE_LOG(LogEstuary, Warning, TEXT("[Bridge] bot_animation: non-object payload — using safe defaults"));
                    AsyncTask(ENamedThreads::GameThread,
                        [WeakSelf, Parsed]()
                        {
                            check(IsInGameThread());
                            TSharedPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin();
                            if (!Pinned.IsValid() || !Pinned->Impl.IsValid() || Pinned->Impl->bShuttingDown) { return; }
                            if (Pinned->OnBotAnimationSink) { Pinned->OnBotAnimationSink(Parsed); }
                        });
                    return;
                }

                const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
                if (!Obj.IsValid()) { return; }

                // Parse with exact SDK_CONTRACT field names. FEstuaryBotAnimation fields:
                //   message_id    -> MessageId
                //   sequence      -> FrameIndex   (-1 on terminator)
                //   time_code_sec -> TimeCodeSec  (seconds from utterance start)
                //   weights       -> Weights      (JSON object { name: float })
                //   is_final      -> bIsFinal
                //   emit_epoch_ms -> EmitEpochMs  (diagnostic; optional)
                Obj->TryGetStringField(TEXT("message_id"), Parsed.MessageId);
                Obj->TryGetBoolField  (TEXT("is_final"),   Parsed.bIsFinal);
                {
                    double SeqD = -1.0;
                    if (Obj->TryGetNumberField(TEXT("sequence"), SeqD))
                    {
                        Parsed.FrameIndex = static_cast<int32>(SeqD);
                    }
                }
                {
                    double TcD = 0.0;
                    if (Obj->TryGetNumberField(TEXT("time_code_sec"), TcD))
                    {
                        Parsed.TimeCodeSec = static_cast<float>(TcD);
                    }
                }
                {
                    // emit_epoch_ms is wall-clock millis (well below 2^53), so a double
                    // pass-through preserves precision.
                    double EmitMsD = 0.0;
                    if (Obj->TryGetNumberField(TEXT("emit_epoch_ms"), EmitMsD))
                    {
                        Parsed.EmitEpochMs = static_cast<int64>(EmitMsD);
                    }
                }

                // Parse blendshape weights — wire shape is a JSON OBJECT keyed by
                // ARKit-52 name, NOT an array of {name,weight} pairs. Domain struct
                // also uses TMap<FString,float>.
                const TSharedPtr<FJsonObject>* WeightsObjPtr = nullptr;
                if (Obj->TryGetObjectField(TEXT("weights"), WeightsObjPtr)
                    && WeightsObjPtr
                    && WeightsObjPtr->IsValid())
                {
                    for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : (*WeightsObjPtr)->Values)
                    {
                        if (!Kv.Value.IsValid()) { continue; }
                        double W = 0.0;
                        if (Kv.Value->TryGetNumber(W))
                        {
                            Parsed.Weights.Add(Kv.Key, static_cast<float>(W));
                        }
                    }
                }

                AsyncTask(ENamedThreads::GameThread,
                    [WeakSelf, Parsed]()
                    {
                        check(IsInGameThread());
                        TSharedPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin();
                        if (!Pinned.IsValid() || !Pinned->Impl.IsValid() || Pinned->Impl->bShuttingDown) { return; }
                        if (Pinned->OnBotAnimationSink) { Pinned->OnBotAnimationSink(Parsed); }
                    });
            },
            TEXT("/sdk"));
    }

    // bot_pose — GestureSync-driven body-animation frames at 30 fps (body_animation_stream).
    // 63 MHR wire bones per frame (LOCAL quaternions [x,y,z,w] in the MHR canonical
    // frame) plus the root-position delta; sequence==-1 / bones==[] / is_final==true
    // marks the terminator (filtered by subsystem). The parse loop iterates
    // BonesArr->Num(); the receiver enforces the ==63 count gate at ingest.
    {
        TWeakPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("bot_pose"),
            [WeakSelf](const FString& /*EventName*/, const TSharedPtr<FJsonValue>& Value)
            {
                // Parse on getnamo's worker thread — safe: pure C++ structs, no UObject.
                FEstuaryBotPose Parsed;

                if (!Value.IsValid() || Value->Type != EJson::Object)
                {
                    UE_LOG(LogEstuary, Warning, TEXT("[Bridge] bot_pose: non-object payload — using safe defaults"));
                    AsyncTask(ENamedThreads::GameThread,
                        [WeakSelf, Parsed]()
                        {
                            check(IsInGameThread());
                            TSharedPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin();
                            if (!Pinned.IsValid() || !Pinned->Impl.IsValid() || Pinned->Impl->bShuttingDown) { return; }
                            if (Pinned->OnBotPoseSink) { Pinned->OnBotPoseSink(Parsed); }
                        });
                    return;
                }

                const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
                if (!Obj.IsValid()) { return; }

                // Parse with exact SDK_CONTRACT field names. FEstuaryBotPose fields:
                //   message_id          -> MessageId
                //   sequence            -> FrameIndex   (-1 on terminator)
                //   time_code_sec       -> TimeCodeSec
                //   fps                 -> Fps          (default 30)
                //   bones               -> Bones        (array of {name, quat:[x,y,z,w]})
                //   hips_world          -> HipsWorld    (3-element array of numbers)
                //   emit_epoch_ms       -> EmitEpochMs
                //   is_final            -> bIsFinal
                Obj->TryGetStringField(TEXT("message_id"), Parsed.MessageId);
                Obj->TryGetBoolField  (TEXT("is_final"),   Parsed.bIsFinal);
                {
                    double SeqD = -1.0;
                    if (Obj->TryGetNumberField(TEXT("sequence"), SeqD))
                    {
                        Parsed.FrameIndex = static_cast<int32>(SeqD);
                    }
                }
                {
                    double TcD = 0.0;
                    if (Obj->TryGetNumberField(TEXT("time_code_sec"), TcD))
                    {
                        Parsed.TimeCodeSec = static_cast<float>(TcD);
                    }
                }
                {
                    double FpsD = 30.0;
                    if (Obj->TryGetNumberField(TEXT("fps"), FpsD))
                    {
                        Parsed.Fps = static_cast<int32>(FpsD);
                    }
                }
                {
                    // emit_epoch_ms is wall-clock millis (well below 2^53), so a double
                    // pass-through preserves precision.
                    double EmitMsD = 0.0;
                    if (Obj->TryGetNumberField(TEXT("emit_epoch_ms"), EmitMsD))
                    {
                        Parsed.EmitEpochMs = static_cast<int64>(EmitMsD);
                    }
                }

                // Parse bones: wire shape is an array of objects, each {name: string, quat: [x,y,z,w]}.
                const TArray<TSharedPtr<FJsonValue>>* BonesArr = nullptr;
                if (Obj->TryGetArrayField(TEXT("bones"), BonesArr) && BonesArr)
                {
                    Parsed.Bones.Reserve(BonesArr->Num());
                    for (const TSharedPtr<FJsonValue>& BoneVal : *BonesArr)
                    {
                        if (!BoneVal.IsValid() || BoneVal->Type != EJson::Object)
                        {
                            UE_LOG(LogEstuary, Verbose,
                                TEXT("[Bridge] bot_pose: skipping non-object bone entry."));
                            continue;
                        }
                        const TSharedPtr<FJsonObject> BoneObj = BoneVal->AsObject();
                        if (!BoneObj.IsValid()) { continue; }

                        FEstuaryBotPoseBone Bone;
                        if (!BoneObj->TryGetStringField(TEXT("name"), Bone.Name))
                        {
                            UE_LOG(LogEstuary, Verbose,
                                TEXT("[Bridge] bot_pose: skipping bone missing 'name' field."));
                            continue;
                        }

                        const TArray<TSharedPtr<FJsonValue>>* QuatArr = nullptr;
                        if (!BoneObj->TryGetArrayField(TEXT("quat"), QuatArr) || !QuatArr || QuatArr->Num() < 4)
                        {
                            UE_LOG(LogEstuary, Verbose,
                                TEXT("[Bridge] bot_pose: skipping bone '%s' with malformed quat array."),
                                *Bone.Name);
                            continue;
                        }

                        double Qx = 0.0, Qy = 0.0, Qz = 0.0, Qw = 1.0;
                        (*QuatArr)[0]->TryGetNumber(Qx);
                        (*QuatArr)[1]->TryGetNumber(Qy);
                        (*QuatArr)[2]->TryGetNumber(Qz);
                        (*QuatArr)[3]->TryGetNumber(Qw);
                        Bone.Quat = FVector4(Qx, Qy, Qz, Qw);

                        Parsed.Bones.Add(MoveTemp(Bone));
                    }
                }

                // Parse hips_world (3-element array of numbers).
                {
                    const TArray<TSharedPtr<FJsonValue>>* HipsWorldArr = nullptr;
                    if (Obj->TryGetArrayField(TEXT("hips_world"), HipsWorldArr)
                        && HipsWorldArr && HipsWorldArr->Num() >= 3)
                    {
                        double Hx = 0.0, Hy = 0.0, Hz = 0.0;
                        (*HipsWorldArr)[0]->TryGetNumber(Hx);
                        (*HipsWorldArr)[1]->TryGetNumber(Hy);
                        (*HipsWorldArr)[2]->TryGetNumber(Hz);
                        Parsed.HipsWorld = FVector(Hx, Hy, Hz);
                    }
                }

                AsyncTask(ENamedThreads::GameThread,
                    [WeakSelf, Parsed]()
                    {
                        check(IsInGameThread());
                        TSharedPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin();
                        if (!Pinned.IsValid() || !Pinned->Impl.IsValid() || Pinned->Impl->bShuttingDown) { return; }
                        if (Pinned->OnBotPoseSink) { Pinned->OnBotPoseSink(Parsed); }
                    });
            },
            TEXT("/sdk"));
    }

    // -----------------------------------------------------------------------
    // Phase 8: LiveKit server-to-client event subscriptions (LK-03, LK-04).
    // SDK_CONTRACT §voice_livekit Server→Client events.
    // -----------------------------------------------------------------------

    // livekit_token — server sends {token, url, room} in response to our request.
    // T-08-03: Token is JWT — parse it but never log it.
    {
        TWeakPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("livekit_token"),
            [WeakSelf](const FString& /*EventName*/, const TSharedPtr<FJsonValue>& Value)
            {
                FString Token, Url, Room;

                if (Value.IsValid() && Value->Type == EJson::Object)
                {
                    const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
                    if (Obj.IsValid())
                    {
                        // Exact SDK_CONTRACT §voice_livekit field names.
                        Obj->TryGetStringField(TEXT("token"), Token);
                        Obj->TryGetStringField(TEXT("url"),   Url);
                        Obj->TryGetStringField(TEXT("room"),  Room);
                    }
                }

                if (Token.IsEmpty())
                {
                    UE_LOG(LogEstuary, Warning,
                        TEXT("[Bridge] livekit_token: received empty token — ignoring."));
                    return;
                }

                // T-08-03: Log url + room only. Never log the token string.
                UE_LOG(LogEstuary, Verbose,
                    TEXT("[Bridge] livekit_token received: url=%s room=%s"), *Url, *Room);

                AsyncTask(ENamedThreads::GameThread,
                    [WeakSelf, Token, Url, Room]()
                    {
                        check(IsInGameThread());
                        TSharedPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin();
                        if (!Pinned.IsValid() || !Pinned->Impl.IsValid() || Pinned->Impl->bShuttingDown) { return; }
                        if (Pinned->OnLiveKitTokenSink)
                        {
                            Pinned->OnLiveKitTokenSink(Token, Url, Room);
                        }
                    });
            },
            TEXT("/sdk"));
    }

    // livekit_ready — server signals the room is ready for the client to join.
    {
        TWeakPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("livekit_ready"),
            [WeakSelf](const FString& /*EventName*/, const TSharedPtr<FJsonValue>& Value)
            {
                FString Room;

                if (Value.IsValid() && Value->Type == EJson::Object)
                {
                    const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
                    if (Obj.IsValid())
                    {
                        Obj->TryGetStringField(TEXT("room"), Room);
                    }
                }

                UE_LOG(LogEstuary, Log, TEXT("[Bridge] livekit_ready received: room=%s"), *Room);

                AsyncTask(ENamedThreads::GameThread,
                    [WeakSelf, Room]()
                    {
                        check(IsInGameThread());
                        TSharedPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin();
                        if (!Pinned.IsValid() || !Pinned->Impl.IsValid() || Pinned->Impl->bShuttingDown) { return; }
                        if (Pinned->OnLiveKitReadySink)
                        {
                            Pinned->OnLiveKitReadySink(Room);
                        }
                    });
            },
            TEXT("/sdk"));
    }

    // livekit_error — server signals a LiveKit-side error.
    {
        TWeakPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("livekit_error"),
            [WeakSelf](const FString& /*EventName*/, const TSharedPtr<FJsonValue>& Value)
            {
                FString ErrorMsg;

                if (Value.IsValid() && Value->Type == EJson::Object)
                {
                    const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
                    if (Obj.IsValid())
                    {
                        Obj->TryGetStringField(TEXT("error"), ErrorMsg);
                    }
                }

                UE_LOG(LogEstuary, Warning, TEXT("[Bridge] livekit_error received: %s"), *ErrorMsg);

                AsyncTask(ENamedThreads::GameThread,
                    [WeakSelf, ErrorMsg]()
                    {
                        check(IsInGameThread());
                        TSharedPtr<FEstuarySocketIOBridge, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin();
                        if (!Pinned.IsValid() || !Pinned->Impl.IsValid() || Pinned->Impl->bShuttingDown) { return; }
                        if (Pinned->OnLiveKitErrorSink)
                        {
                            Pinned->OnLiveKitErrorSink(ErrorMsg);
                        }
                    });
            },
            TEXT("/sdk"));
    }

    // session_timeout — server idle reap (SDK_CONTRACT §session_timeout).
    // The gateway force-closes the socket IMMEDIATELY after this event; the
    // subsystem uses this sink to suppress its auto-reconnect for that close
    // (re-authenticating would re-establish billed voice resources in a loop).
    {
        TWeakPtr<FEstuarySocketIOBridge> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("session_timeout"),
            [WeakSelf](const FString& EventName, const TSharedPtr<FJsonValue>& EventData)
            {
                FEstuarySessionTimeout Out;
                TSharedPtr<FJsonObject> JsonObj;
                if (EventData.IsValid() && EventData->Type == EJson::Object)
                {
                    JsonObj = EventData->AsObject();
                }

                if (JsonObj.IsValid())
                {
                    JsonObj->TryGetStringField(TEXT("reason"),          Out.Reason);
                    JsonObj->TryGetNumberField(TEXT("idle_seconds"),    Out.IdleSeconds);
                    JsonObj->TryGetNumberField(TEXT("timeout_seconds"), Out.TimeoutSeconds);

                    TWeakPtr<FEstuarySocketIOBridge> WeakSelfCopy = WeakSelf;
                    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : JsonObj->Values)
                    {
                        if (!KnownSessionTimeoutFields.Contains(Field.Key))
                        {
                            TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelfCopy.Pin();
                            if (Self)
                            {
                                FScopeLock Lock(&Self->Impl->WarnedLock);
                                FString CacheKey = FString::Printf(TEXT("session_timeout:%s"), *Field.Key);
                                if (!Self->Impl->WarnedUnknownFields.Contains(CacheKey))
                                {
                                    Self->Impl->WarnedUnknownFields.Add(CacheKey);
                                    UE_LOG(LogEstuary, Verbose,
                                        TEXT("[Bridge] session_timeout: unknown field '%s' (schema drift?)"), *Field.Key);
                                }
                            }
                        }
                    }
                }

                UE_LOG(LogEstuary, Warning,
                    TEXT("[Bridge] session_timeout received: reason=%s idle=%ds timeout=%ds — server will disconnect"),
                    *Out.Reason, Out.IdleSeconds, Out.TimeoutSeconds);

                AsyncTask(ENamedThreads::GameThread, [WeakSelf, Out]()
                {
                    check(IsInGameThread());
                    TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelf.Pin();
                    if (!Self || Self->Impl->bShuttingDown) { return; }
                    if (Self->OnSessionTimeoutSink) { Self->OnSessionTimeoutSink(Out); }
                });
            },
            TEXT("/sdk"));
    }

    // voice_timeout — voice-lane idle release (SDK_CONTRACT §voice_timeout).
    // Arrives on the STILL-OPEN socket; no disconnect follows (opposite of
    // session_timeout — never wire this into reconnect suppression). The
    // subsystem releases local voice resources only; text chat continues.
    {
        TWeakPtr<FEstuarySocketIOBridge> WeakSelf = AsWeak();
        Impl->Socket->OnEvent(
            TEXT("voice_timeout"),
            [WeakSelf](const FString& EventName, const TSharedPtr<FJsonValue>& EventData)
            {
                FEstuaryVoiceTimeout Out;
                TSharedPtr<FJsonObject> JsonObj;
                if (EventData.IsValid() && EventData->Type == EJson::Object)
                {
                    JsonObj = EventData->AsObject();
                }

                if (JsonObj.IsValid())
                {
                    JsonObj->TryGetStringField(TEXT("reason"),          Out.Reason);
                    JsonObj->TryGetNumberField(TEXT("idle_seconds"),    Out.IdleSeconds);
                    JsonObj->TryGetNumberField(TEXT("timeout_seconds"), Out.TimeoutSeconds);

                    TWeakPtr<FEstuarySocketIOBridge> WeakSelfCopy = WeakSelf;
                    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : JsonObj->Values)
                    {
                        if (!KnownVoiceTimeoutFields.Contains(Field.Key))
                        {
                            TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelfCopy.Pin();
                            if (Self)
                            {
                                FScopeLock Lock(&Self->Impl->WarnedLock);
                                FString CacheKey = FString::Printf(TEXT("voice_timeout:%s"), *Field.Key);
                                if (!Self->Impl->WarnedUnknownFields.Contains(CacheKey))
                                {
                                    Self->Impl->WarnedUnknownFields.Add(CacheKey);
                                    UE_LOG(LogEstuary, Verbose,
                                        TEXT("[Bridge] voice_timeout: unknown field '%s' (schema drift?)"), *Field.Key);
                                }
                            }
                        }
                    }
                }

                UE_LOG(LogEstuary, Warning,
                    TEXT("[Bridge] voice_timeout received: reason=%s idle=%ds timeout=%ds — socket stays connected"),
                    *Out.Reason, Out.IdleSeconds, Out.TimeoutSeconds);

                AsyncTask(ENamedThreads::GameThread, [WeakSelf, Out]()
                {
                    check(IsInGameThread());
                    TSharedPtr<FEstuarySocketIOBridge> Self = WeakSelf.Pin();
                    if (!Self || Self->Impl->bShuttingDown) { return; }
                    if (Self->OnVoiceTimeoutSink) { Self->OnVoiceTimeoutSink(Out); }
                });
            },
            TEXT("/sdk"));
    }

    // -----------------------------------------------------------------------
    // Fire the actual socket connect.
    // v2.11.0 Connect signature: Connect(AddressAndPort) or Connect(FSIOConnectParams).
    // There is NO 2-arg (URL, namespace) overload. Namespace routing is handled by
    // the per-event namespace parameter on OnEvent/Emit calls above (each subscribes
    // to "/sdk" explicitly), which the underlying sio_client lazily joins on first
    // emit/listen — the v2.11.0 idiomatic pattern.
    // -----------------------------------------------------------------------
    UE_LOG(LogEstuary, Log, TEXT("[Bridge] Initiating socket connect to %s"), *ServerUrl);
    Impl->Socket->Connect(ServerUrl);
}

// ============================================================================
// EmitUserText (Plan 03-02; TEXT-02)
// ============================================================================

void FEstuarySocketIOBridge::EmitUserText(const FEstuaryUserText& Payload)
{
    check(IsInGameThread());

    if (!Impl.IsValid() || !Impl->Socket.IsValid() || !Impl->Socket->bIsConnected)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Bridge] EmitUserText: not connected — drop (message_id=%s)"),
            *Payload.MessageId);
        return;
    }

    // Per SDK_CONTRACT §text_chat: { "text": string, "textOnly": bool }
    // Note camelCase `textOnly` (NOT snake_case) — verified against SDK_CONTRACT.
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("text"),     Payload.Text);
    Obj->SetBoolField  (TEXT("textOnly"), Payload.bTextOnly);

    UE_LOG(LogEstuary, Verbose,
        TEXT("[Bridge] emit text (message_id=%s chars=%d text_only=%d)"),
        *Payload.MessageId, Payload.Text.Len(), Payload.bTextOnly ? 1 : 0);

    Impl->Socket->Emit(TEXT("text"), Obj, nullptr, TEXT("/sdk"));
}

// ============================================================================
// EmitSayLine (SDK_CONTRACT §say_line)
// ============================================================================

void FEstuarySocketIOBridge::EmitSayLine(const FString& Text, bool bTextOnly)
{
    check(IsInGameThread());

    if (!Impl.IsValid() || !Impl->Socket.IsValid() || !Impl->Socket->bIsConnected)
    {
        UE_LOG(LogEstuary, Warning, TEXT("[Bridge] EmitSayLine: not connected — drop"));
        return;
    }

    // Per SDK_CONTRACT §say_line: { "text": string, "text_only"?: bool }
    // Note SNAKE_case `text_only` here — distinct from `text`'s camelCase `textOnly`.
    // Always send the flag (default false) so the server's null-vs-absent handling
    // is uniform. The server auto-interrupts any in-flight response and mints its
    // OWN message_id; the response arrives via the existing bot_response/bot_voice path.
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("text"),      Text);
    Obj->SetBoolField  (TEXT("text_only"), bTextOnly);

    UE_LOG(LogEstuary, Verbose,
        TEXT("[Bridge] emit say_line (chars=%d text_only=%d)"),
        Text.Len(), bTextOnly ? 1 : 0);

    Impl->Socket->Emit(TEXT("say_line"), Obj, nullptr, TEXT("/sdk"));
}

// ============================================================================
// EmitInterrupt (Plan 03-02; TEXT-04)
// ============================================================================

void FEstuarySocketIOBridge::EmitInterrupt(const FEstuaryInterruptPayload& Payload)
{
    check(IsInGameThread());

    if (!Impl.IsValid() || !Impl->Socket.IsValid() || !Impl->Socket->bIsConnected)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Bridge] EmitInterrupt: not connected — drop (message_id=%s)"),
            *Payload.MessageId);
        return;
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    if (!Payload.MessageId.IsEmpty())
    {
        // Contract: message_id is OPTIONAL. Omit key entirely when empty so server's
        // null-vs-empty-string handling is uniform (ts-sdk convention).
        Obj->SetStringField(TEXT("message_id"), Payload.MessageId);
    }

    UE_LOG(LogEstuary, Log, TEXT("[Bridge] emit client_interrupt (message_id=%s)"), *Payload.MessageId);
    Impl->Socket->Emit(TEXT("client_interrupt"), Obj, nullptr, TEXT("/sdk"));
}

// ============================================================================
// EmitUpdatePreferences (Plan 03-02; TEXT-05)
// ============================================================================

void FEstuarySocketIOBridge::EmitUpdatePreferences(const FEstuaryPreferences& Payload)
{
    check(IsInGameThread());

    if (!Impl.IsValid() || !Impl->Socket.IsValid() || !Impl->Socket->bIsConnected)
    {
        UE_LOG(LogEstuary, Warning, TEXT("[Bridge] EmitUpdatePreferences: not connected — drop"));
        return;
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    // Canonical camelCase wire field (SDK_CONTRACT §preferences).
    Obj->SetBoolField(TEXT("enableVisionAcknowledgment"), Payload.bEnableVisionAcknowledgment);

    // Forward-compat bag — verbatim keys/values.
    // Heuristic: if a value is "true"/"false" treat as bool; if pure digits with optional
    // leading '-' treat as number; otherwise string. This mirrors the ts-sdk leniency and
    // keeps the Blueprint UX (TMap<FString,FString>) portable.
    for (const TPair<FString, FString>& Kv : Payload.ExtraPreferences)
    {
        const FString& K = Kv.Key;
        const FString& V = Kv.Value;

        if (V.Equals(TEXT("true"),  ESearchCase::IgnoreCase)) { Obj->SetBoolField(K, true);  }
        else if (V.Equals(TEXT("false"), ESearchCase::IgnoreCase)) { Obj->SetBoolField(K, false); }
        else if (V.IsNumeric())                                    { Obj->SetNumberField(K, FCString::Atof(*V)); }
        else                                                       { Obj->SetStringField(K, V); }
    }

    UE_LOG(LogEstuary, Log,
        TEXT("[Bridge] emit update_preferences (enableVisionAck=%d extra_keys=%d)"),
        Payload.bEnableVisionAcknowledgment ? 1 : 0, Payload.ExtraPreferences.Num());

    Impl->Socket->Emit(TEXT("update_preferences"), Obj, nullptr, TEXT("/sdk"));
}

// ============================================================================
// EmitAudioPlaybackComplete (AUDIO-05)
// ============================================================================

void FEstuarySocketIOBridge::EmitAudioPlaybackComplete(const FEstuaryAudioPlaybackComplete& Payload)
{
    check(IsInGameThread());

    if (!Impl.IsValid() || !Impl->Socket.IsValid() || !Impl->Socket->bIsConnected)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Bridge] EmitAudioPlaybackComplete: not connected — drop (message_id=%s)"),
            *Payload.MessageId);
        return;
    }

    // Per SDK_CONTRACT §audio_playback_tracking: emit "audio_playback_complete" with
    // {message_id}. message_id is optional — emit empty string if unknown (per contract).
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("message_id"), Payload.MessageId);

    UE_LOG(LogEstuary, Log,
        TEXT("[Bridge] emit audio_playback_complete (message_id=%s)"), *Payload.MessageId);

    Impl->Socket->Emit(TEXT("audio_playback_complete"), Obj, nullptr, TEXT("/sdk"));
}

// ============================================================================
// EmitStartVoice (Phase 5 — MIC-03)
// ============================================================================

void FEstuarySocketIOBridge::EmitStartVoice()
{
    check(IsInGameThread());

    if (!Impl.IsValid() || !Impl->Socket.IsValid() || !Impl->Socket->bIsConnected)
    {
        UE_LOG(LogEstuary, Warning, TEXT("[Bridge] EmitStartVoice: not connected — drop"));
        return;
    }

    // Per SDK_CONTRACT §voice_websocket: start_voice payload is null.
    // Emit with an empty JSON object {} as the closest null-equivalent in getnamo v2.11.0.
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    UE_LOG(LogEstuary, Log, TEXT("[Bridge] emit start_voice"));
    Impl->Socket->Emit(TEXT("start_voice"), Obj, nullptr, TEXT("/sdk"));
}

// ============================================================================
// EmitStreamAudio (Phase 5 — MIC-03)
// ============================================================================

void FEstuarySocketIOBridge::EmitStreamAudio(const FEstuaryStreamAudio& Payload)
{
    check(IsInGameThread());

    if (!Impl.IsValid() || !Impl->Socket.IsValid() || !Impl->Socket->bIsConnected)
    {
        UE_LOG(LogEstuary, Warning, TEXT("[Bridge] EmitStreamAudio: not connected — drop"));
        return;
    }

    // Per SDK_CONTRACT §voice_websocket: stream_audio payload is {"audio": string (base64)}.
    // T-05-01-C mitigation: do NOT log the audio field value — log chunk byte length only.
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("audio"), Payload.Audio);

    UE_LOG(LogEstuary, Verbose,
        TEXT("[Bridge] emit stream_audio (base64_len=%d)"), Payload.Audio.Len());

    Impl->Socket->Emit(TEXT("stream_audio"), Obj, nullptr, TEXT("/sdk"));
}

// ============================================================================
// EmitStopVoice (Phase 5 — MIC-03)
// ============================================================================

void FEstuarySocketIOBridge::EmitStopVoice()
{
    check(IsInGameThread());

    if (!Impl.IsValid() || !Impl->Socket.IsValid() || !Impl->Socket->bIsConnected)
    {
        UE_LOG(LogEstuary, Warning, TEXT("[Bridge] EmitStopVoice: not connected — drop"));
        return;
    }

    // Per SDK_CONTRACT §voice_websocket: stop_voice payload is null.
    // Emit with an empty JSON object {} as the closest null-equivalent in getnamo v2.11.0.
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    UE_LOG(LogEstuary, Log, TEXT("[Bridge] emit stop_voice"));
    Impl->Socket->Emit(TEXT("stop_voice"), Obj, nullptr, TEXT("/sdk"));
}

// ============================================================================
// EmitCameraImage (Phase 7 — CAM-01, CAM-02)
// ============================================================================

void FEstuarySocketIOBridge::EmitCameraImage(const FEstuaryCameraImage& Payload)
{
    check(IsInGameThread()); // Pitfall 1: emit must be on the game thread.

    if (!Impl.IsValid() || !Impl->Socket.IsValid() || !Impl->Socket->bIsConnected)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[Bridge] EmitCameraImage: not connected — drop (request_id=%s)"),
            *Payload.RequestId);
        return;
    }

    // Build wire payload with EXACT SDK_CONTRACT snake_case keys.
    // SDK_CONTRACT §vision_camera: { image, mime_type, request_id?, text?, sample_rate? }
    // T-07-02: NEVER log ImageJpegBase64 — security boundary (base64 image data).
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    // "image" — required wire field.
    Obj->SetStringField(TEXT("image"), Payload.ImageJpegBase64);

    // "mime_type" — required wire field. Use payload value (default "image/jpeg").
    Obj->SetStringField(TEXT("mime_type"), Payload.MimeType.IsEmpty()
        ? TEXT("image/jpeg") : Payload.MimeType);

    // "request_id" — optional per contract. Omit if empty (consistent with ts-sdk pattern).
    if (!Payload.RequestId.IsEmpty())
    {
        Obj->SetStringField(TEXT("request_id"), Payload.RequestId);
    }

    // "text" — optional prompt context. Omit if empty.
    if (!Payload.Text.IsEmpty())
    {
        Obj->SetStringField(TEXT("text"), Payload.Text);
    }

    // "sample_rate" — omit when 0 (sentinel meaning "not specified").
    if (Payload.SampleRate != 0)
    {
        Obj->SetNumberField(TEXT("sample_rate"), static_cast<double>(Payload.SampleRate));
    }

    // T-07-02: Log ONLY non-sensitive metadata. Never log the image data.
    UE_LOG(LogEstuary, Verbose,
        TEXT("[Bridge] EmitCameraImage request_id=%s width=%d height=%d mime=%s"),
        *Payload.RequestId, Payload.Width, Payload.Height, *Payload.MimeType);

    Impl->Socket->Emit(TEXT("camera_image"), Obj, nullptr, TEXT("/sdk"));
}

// ============================================================================
// EmitUserPos — GestureSync dyadic conditioning (paper p_y)
// SDK_CONTRACT §user_pos — REQUIRED-FOR-BODY when GestureSync provider is active.
// High-frequency event (~30 Hz); per-frame logging would dominate output.
// ============================================================================

void FEstuarySocketIOBridge::EmitUserPos(float X, float Y)
{
    check(IsInGameThread());

    if (!Impl.IsValid() || !Impl->Socket.IsValid() || !Impl->Socket->bIsConnected)
    {
        // Silent drop — this fires every frame during a GestureSync session and a
        // disconnect is already noisy via session state callbacks.
        return;
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("x"), static_cast<double>(X));
    Obj->SetNumberField(TEXT("y"), static_cast<double>(Y));
    Impl->Socket->Emit(TEXT("user_pos"), Obj, nullptr, TEXT("/sdk"));
}

// ============================================================================
// LiveKit emit methods (Phase 8 — LK-03, LK-04)
// SDK_CONTRACT §voice_livekit Client→Server events (all null/empty payload).
// ============================================================================

void FEstuarySocketIOBridge::EmitLiveKitTokenRequest()
{
    check(IsInGameThread());

    if (!Impl.IsValid() || !Impl->Socket.IsValid() || !Impl->Socket->bIsConnected)
    {
        UE_LOG(LogEstuary, Warning, TEXT("[Bridge] EmitLiveKitTokenRequest: not connected — drop"));
        return;
    }

    // Per SDK_CONTRACT §voice_livekit: livekit_token event, null payload.
    // Emit with an empty JSON object {} as the closest null-equivalent in getnamo v2.11.0.
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    UE_LOG(LogEstuary, Log, TEXT("[Bridge] emit livekit_token (request)"));
    Impl->Socket->Emit(TEXT("livekit_token"), Obj, nullptr, TEXT("/sdk"));
}

void FEstuarySocketIOBridge::EmitLiveKitJoin()
{
    check(IsInGameThread());

    if (!Impl.IsValid() || !Impl->Socket.IsValid() || !Impl->Socket->bIsConnected)
    {
        UE_LOG(LogEstuary, Warning, TEXT("[Bridge] EmitLiveKitJoin: not connected — drop"));
        return;
    }

    // Per SDK_CONTRACT §voice_livekit: livekit_join event, null payload.
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    UE_LOG(LogEstuary, Log, TEXT("[Bridge] emit livekit_join"));
    Impl->Socket->Emit(TEXT("livekit_join"), Obj, nullptr, TEXT("/sdk"));
}

void FEstuarySocketIOBridge::EmitLiveKitLeave()
{
    check(IsInGameThread());

    if (!Impl.IsValid() || !Impl->Socket.IsValid() || !Impl->Socket->bIsConnected)
    {
        UE_LOG(LogEstuary, Warning, TEXT("[Bridge] EmitLiveKitLeave: not connected — drop"));
        return;
    }

    // Per SDK_CONTRACT §voice_livekit: livekit_leave event, null payload.
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    UE_LOG(LogEstuary, Log, TEXT("[Bridge] emit livekit_leave"));
    Impl->Socket->Emit(TEXT("livekit_leave"), Obj, nullptr, TEXT("/sdk"));
}

#else // !WITH_ESTUARY_SOCKETIO
// =============================================================================
// Stub mode — SocketIOClient plugin (getnamo) is not installed.
// FEstuarySocketIOBridge compiles to no-op stubs so the rest of the SDK can build.
// The editor auto-installer (Source/EstuaryEditor/Private/EstuaryAutoInstaller.cpp)
// detects the missing plugin on editor startup and prompts the user to git-clone it.
// After install + restart, WITH_ESTUARY_SOCKETIO=1 and full functionality returns.
// =============================================================================

// Empty pimpl in stub mode — TUniquePtr<FImpl> in the header still resolves cleanly.
struct FEstuarySocketIOBridge::FImpl {};

namespace
{
    /** One-time-per-call-site warning so the log isn't spammed on tight loops. */
    void WarnStubOnce(const TCHAR* Method)
    {
        static TSet<FString> Warned;
        const FString Key(Method);
        if (!Warned.Contains(Key))
        {
            Warned.Add(Key);
            UE_LOG(LogEstuary, Warning,
                TEXT("[Bridge stub] %s called but SocketIOClient plugin is not installed. ")
                TEXT("Open the editor: the Estuary auto-installer will prompt you to clone it ")
                TEXT("(or run: git clone --depth 1 --branch v2.11.0 ")
                TEXT("https://github.com/getnamo/SocketIOClient-Unreal.git Plugins/SocketIOClient)."),
                Method);
        }
    }
}

FEstuarySocketIOBridge::FEstuarySocketIOBridge()
    : Impl(MakeUnique<FImpl>())
{
    UE_LOG(LogEstuary, Warning,
        TEXT("[Bridge stub] FEstuarySocketIOBridge constructed in STUB MODE — ")
        TEXT("SocketIOClient plugin not installed. Connect/voice/etc. will be no-ops ")
        TEXT("until the dependency is added (auto-installer prompts on editor startup)."));
}

FEstuarySocketIOBridge::~FEstuarySocketIOBridge() = default;

void FEstuarySocketIOBridge::Connect(const FString& /*ServerUrl*/, const FEstuaryAuthPayload& /*Auth*/)
{
    WarnStubOnce(TEXT("Connect"));
    // Synthesize a transport error so the subsystem state machine transitions to Error
    // cleanly instead of hanging in Connecting.
    if (OnTransportError)
    {
        OnTransportError(TEXT("SocketIOClient plugin not installed (Estuary stub mode). ")
                         TEXT("The editor auto-installer will prompt to install it on next launch."));
    }
}

void FEstuarySocketIOBridge::Disconnect()                                                    { WarnStubOnce(TEXT("Disconnect")); }
bool FEstuarySocketIOBridge::IsConnected() const                                             { return false; }

void FEstuarySocketIOBridge::EmitUserText(const FEstuaryUserText&)                           { WarnStubOnce(TEXT("EmitUserText")); }
void FEstuarySocketIOBridge::EmitSayLine(const FString&, bool)                               { WarnStubOnce(TEXT("EmitSayLine")); }
void FEstuarySocketIOBridge::EmitInterrupt(const FEstuaryInterruptPayload&)                  { WarnStubOnce(TEXT("EmitInterrupt")); }
void FEstuarySocketIOBridge::EmitUpdatePreferences(const FEstuaryPreferences&)               { WarnStubOnce(TEXT("EmitUpdatePreferences")); }
void FEstuarySocketIOBridge::EmitAudioPlaybackComplete(const FEstuaryAudioPlaybackComplete&) { WarnStubOnce(TEXT("EmitAudioPlaybackComplete")); }
void FEstuarySocketIOBridge::EmitStartVoice()                                                { WarnStubOnce(TEXT("EmitStartVoice")); }
void FEstuarySocketIOBridge::EmitStreamAudio(const FEstuaryStreamAudio&)                     { WarnStubOnce(TEXT("EmitStreamAudio")); }
void FEstuarySocketIOBridge::EmitStopVoice()                                                 { WarnStubOnce(TEXT("EmitStopVoice")); }
void FEstuarySocketIOBridge::EmitUserPos(float, float)                                        { WarnStubOnce(TEXT("EmitUserPos")); }
void FEstuarySocketIOBridge::EmitCameraImage(const FEstuaryCameraImage&)                     { WarnStubOnce(TEXT("EmitCameraImage")); }
void FEstuarySocketIOBridge::EmitLiveKitTokenRequest()                                       { WarnStubOnce(TEXT("EmitLiveKitTokenRequest")); }
void FEstuarySocketIOBridge::EmitLiveKitJoin()                                               { WarnStubOnce(TEXT("EmitLiveKitJoin")); }
void FEstuarySocketIOBridge::EmitLiveKitLeave()                                              { WarnStubOnce(TEXT("EmitLiveKitLeave")); }

#endif // WITH_ESTUARY_SOCKETIO
