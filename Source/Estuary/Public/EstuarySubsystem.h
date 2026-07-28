// EstuarySubsystem.h — UEstuarySubsystem: UGameInstanceSubsystem owning the Socket.IO
// transport, driving the EEstuarySessionState machine, and exposing the Blueprint API.
//
// Pitfall 23: #pragma once, no CoreMinimal.h, no Engine.h, IWYU-only.
// Pitfall 21: FEstuarySocketIOBridge is forward-declared only — never include the
//             Private bridge header from a Public header.

#pragma once

#include "CoreTypes.h"
#include "UObject/ObjectMacros.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/UnrealString.h"
#include "Containers/Map.h"
#include "Internationalization/Text.h"
#include "Containers/Ticker.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"
#include "EstuaryTypes.h"
#include "EstuaryLiveKitBridge.h"
#include "EstuaryLiveLinkBridge.h"

#include "EstuarySubsystem.generated.h"

// Forward-declare the Private bridge class. Never expose its header here.
class FEstuarySocketIOBridge;

// Forward-declare the character component for registry typing.
// Full header is included in EstuarySubsystem.cpp only (Pitfall 21: no component
// header exposed from the subsystem's public header).
class UEstuaryCharacterComponent;

// Forward-declare the camera component for camera registry typing.
// Full header is included in EstuarySubsystem.cpp only (Pitfall 21).
class UEstuaryCameraComponent;

// ---------------------------------------------------------------------------
// Delegate declarations — one per Blueprint output pin.
// USTRUCT(BlueprintType) payloads keep us under the 8-param cap (Pitfall 16).
// ---------------------------------------------------------------------------

/** Fired when the Estuary session is established and session_info is received. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuarySessionConnected,
    const FEstuarySessionInfo&, SessionInfo);

/** Fired when the gateway rejects the auth payload (invalid api_key, character_id, player_id). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuaryAuthError,
    const FEstuaryAuthError&, AuthError);

/** Fired when the gateway rejects the session due to a server-side policy cap. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuarySessionRejected,
    const FEstuarySessionRejected&, SessionRejected);

/** Fired when the gateway sends a non-terminal `error` event. Session stays open. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuaryError,
    const FEstuaryError&, Error);

/** Fired when the gateway sends `quota_exceeded`. The gateway disconnects right after. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuaryQuotaExceeded,
    const FEstuaryQuotaExceeded&, QuotaExceeded);

/** Fired when the gateway pushes `memory_updated` after background memory extraction. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuaryMemoryUpdated,
    const FEstuaryMemoryUpdated&, MemoryUpdated);

/** Fired when the gateway reaps the session for inactivity (`session_timeout`).
 *  The gateway disconnects right after; the SDK will NOT auto-reconnect. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuarySessionTimeout,
    const FEstuarySessionTimeout&, SessionTimeout);

/** Fired when the gateway releases voice resources for voice inactivity (`voice_timeout`).
 *  The socket stays connected; text chat continues. Local voice state is already released. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuaryVoiceTimeout,
    const FEstuaryVoiceTimeout&, VoiceTimeout);

/** Fired when the server confirms the voice pipeline is live (`livekit_ready`).
 *  Fires after every LiveKit join, including the auto-rejoin on the first
 *  StartListening after a voice_timeout. WebSocket voice has no server ack,
 *  so this never fires in pure-WebSocket mode. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuaryVoiceReady,
    const FString&, Room);

/** Fired on every EEstuarySessionState transition, including Reconnecting and Error. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuarySessionStateChanged,
    EEstuarySessionState, NewState);

// ---------------------------------------------------------------------------
// UEstuarySubsystem
// ---------------------------------------------------------------------------

/**
 * Auto-instantiated Game Instance Subsystem that owns the Estuary Socket.IO session.
 *
 * Access from C++:
 *   UEstuarySubsystem* Sub = GetGameInstance()->GetSubsystem<UEstuarySubsystem>();
 *
 * Access from Blueprint:
 *   GetGameInstance -> GetSubsystemEstuary (auto-generated accessor)
 *
 * Lifecycle:
 *   Initialize() — created when the GameInstance starts (subsystem auto-init).
 *   Deinitialize() — called on GameInstance shutdown / PIE stop.
 *                    Cleans up the bridge and reconnect ticker — no leaked sockets.
 *
 * Thread safety:
 *   All public methods must be called on the game thread.
 *   Bridge sinks fire on the game thread (bridge marshals internally);
 *   check(IsInGameThread()) guards every handler.
 */
UCLASS()
class ESTUARY_API UEstuarySubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:

    // -----------------------------------------------------------------------
    // UGameInstanceSubsystem overrides
    // -----------------------------------------------------------------------

    /** Called once when the GameInstance starts. Initialises state; does NOT open a socket. */
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    /** Called on GameInstance shutdown or PIE stop. Cancels reconnect, tears down the bridge. */
    virtual void Deinitialize() override;

    // -----------------------------------------------------------------------
    // Primary Blueprint API
    // -----------------------------------------------------------------------

    /**
     * Open a Socket.IO v4 session to ServerUrl on the /sdk namespace, then authenticate.
     *
     * ServerUrl is the gateway URL (Socket.IO over WebSocket), e.g.: https://api.estuary-ai.com
     * Auth must have ApiKey, CharacterId, and PlayerId populated.
     *
     * If a session is already Connecting or Connected, this call is a no-op (logs Warning).
     * To reconnect with different credentials, call Disconnect() first.
     *
     * State transitions: Disconnected/Error -> Connecting -> Connected (on session_info)
     *                                                     -> Error      (on auth_error/rejected)
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Transport",
              meta = (DisplayName = "Connect",
                      ToolTip = "Open an Estuary session. Fires OnSessionConnected on success, OnAuthError / OnSessionRejected on failure."))
    void Connect(const FString& ServerUrl, const FEstuaryAuthPayload& Auth);

    /**
     * Close the active session cleanly. Safe to call in any state.
     * Cancels any pending reconnect timer. Transitions to Disconnected.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Transport",
              meta = (DisplayName = "Disconnect",
                      ToolTip = "Close the Estuary session and cancel any pending reconnect."))
    void Disconnect();

    // -----------------------------------------------------------------------
    // Text-chat API — Phase 3 (TEXT-02, TEXT-04, TEXT-05).
    // These forward to the bridge after connectivity validation.
    // Must be called on the game thread.
    // -----------------------------------------------------------------------

    /**
     * Emit the 'text' event to the backend on behalf of a character component.
     *
     * Payload.MessageId must be non-empty (generated by UEstuaryCharacterComponent::SendText).
     * Payload.CharacterId identifies the originating AEstuaryCharacter for response routing.
     * If not connected, logs a Warning and returns without crashing.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|TextChat",
              meta = (DisplayName = "Send User Text",
                      ToolTip = "Emit the text event on behalf of a character. Called by UEstuaryCharacterComponent::SendText."))
    void SendUserText(const FEstuaryUserText& Payload);

    /**
     * Emit the 'say_line' event — script the character to speak prewritten text via TTS
     * (skips the LLM entirely). The server auto-interrupts any in-flight response and mints
     * its OWN message_id, so there is no correlatable id (unlike SendUserText). The response
     * flows back via the existing bot_response / bot_voice path.
     *
     * @param Text       The prewritten line to speak. Empty text is a server-side no-op.
     * @param bTextOnly  When true, only bot_response is returned (no bot_voice audio).
     * If not connected, logs a Warning and returns without crashing.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|TextChat",
              meta = (DisplayName = "Send Say Line",
                      ToolTip = "Emit say_line to script prewritten character speech (TTS only, no LLM). Called by UEstuaryCharacterComponent::SayLine."))
    void SendSayLine(const FString& Text, bool bTextOnly);

    /**
     * Emit the 'client_interrupt' event. Signals the server to abort the in-flight response
     * for Payload.MessageId (or any in-flight response if MessageId is empty).
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|TextChat",
              meta = (DisplayName = "Send Interrupt",
                      ToolTip = "Emit client_interrupt to abort an in-flight bot response."))
    void SendInterrupt(const FEstuaryInterruptPayload& Payload);

    /**
     * Emit the 'update_preferences' event with the given preference flags.
     * Server applies the preferences to subsequent bot responses in this session.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|TextChat",
              meta = (DisplayName = "Send Update Preferences",
                      ToolTip = "Emit update_preferences to modify session-level behavior flags."))
    void SendUpdatePreferences(const FEstuaryPreferences& Payload);

    // -----------------------------------------------------------------------
    // Audio API — Phase 4 (AUDIO-01, AUDIO-05).
    // -----------------------------------------------------------------------

    /**
     * Forward audio_playback_complete to the bridge for server notification.
     * Called by UEstuaryAudioPlayerComponent::TickComponent when the ring buffer
     * drains after receiving the final chunk for a message.
     *
     * Per SDK_CONTRACT §audio_playback_tracking: message_id is optional —
     * emit empty string if unknown.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Audio",
              meta = (DisplayName = "Emit Audio Playback Complete",
                      ToolTip = "Notify the server that audio playback for MessageId has finished. Called automatically by UEstuaryAudioPlayerComponent."))
    void EmitAudioPlaybackComplete(const FEstuaryAudioPlaybackComplete& Payload);

    // -----------------------------------------------------------------------
    // Voice/STT Blueprint API — Phase 5 (MIC-03).
    // Called by UEstuaryMicrophoneComponent; must be on the game thread.
    // Blueprint-callable variants use Blueprint-simple no-arg signatures.
    // C++ callers (UEstuaryMicrophoneComponent) use StartVoiceSession / StopVoiceSession
    // which carry CharacterId for routing (see below).
    // -----------------------------------------------------------------------

    /** Emit 'start_voice'. No-op if not Connected. Blueprint convenience wrapper.
     *  For C++ use, prefer StartVoiceSession(CharacterId). */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Voice",
              meta = (DisplayName = "Send Start Voice",
                      ToolTip = "Emit start_voice to begin a voice capture session. Call before SendStreamAudio."))
    void SendStartVoice();

    /** Emit 'stream_audio' with a single base64 PCM16 chunk. No-op if not Connected. */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Voice",
              meta = (DisplayName = "Send Stream Audio",
                      ToolTip = "Emit stream_audio with one base64 PCM16 chunk. Called each game-thread tick by UEstuaryMicrophoneComponent."))
    void SendStreamAudio(const FEstuaryStreamAudio& Payload);

    /** Emit 'stop_voice'. No-op if not Connected. Blueprint convenience wrapper.
     *  For C++ use, prefer StopVoiceSession(CharacterId). */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Voice",
              meta = (DisplayName = "Send Stop Voice",
                      ToolTip = "Emit stop_voice to end the voice capture session and trigger final STT processing."))
    void SendStopVoice();

    // -----------------------------------------------------------------------
    // Voice session C++ API — Phase 5 (MIC-03).
    // Called directly by UEstuaryMicrophoneComponent. NOT BlueprintCallable
    // (Blueprint uses SetMuted / StartListening on the component instead).
    // -----------------------------------------------------------------------

    /** Begin a voice session for the given character. Emits start_voice and sets
     *  ActiveVoiceCharacterId for stt_response routing. Must be called on game thread. */
    void StartVoiceSession(const FString& CharacterId);

    /** End the voice session. Emits stop_voice and clears ActiveVoiceCharacterId.
     *  Must be called on game thread. */
    void StopVoiceSession(const FString& CharacterId);

    // -----------------------------------------------------------------------
    // Character registry — Phase 3 (TEXT-06 multi-character support).
    // Called from UEstuaryCharacterComponent::BeginPlay and EndPlay.
    // Must be called on the game thread.
    // -----------------------------------------------------------------------

    /**
     * Register a character component for bot_response and interrupt routing.
     * The component's owning AEstuaryCharacter must have a non-empty CharacterId set.
     * Calling this a second time with the same CharacterId overwrites the previous entry
     * (safe; the old component has typically been destroyed by then).
     *
     * Thread safety: check(IsInGameThread()) — call ONLY from BeginPlay or game-thread code.
     */
    void RegisterCharacter(UEstuaryCharacterComponent* Component);

    /**
     * Remove a character component from the registry. Safe to call with nullptr or a
     * stale (destroyed) component — the function validates before touching the map.
     *
     * Thread safety: check(IsInGameThread()) — call ONLY from EndPlay or game-thread code.
     */
    void UnregisterCharacter(UEstuaryCharacterComponent* Component);

    // -----------------------------------------------------------------------
    // Camera component registry — Phase 7 (CAM-03).
    // Called from UEstuaryCameraComponent::BeginPlay and EndPlay.
    // Must be called on the game thread.
    // -----------------------------------------------------------------------

    /** Emit camera_image event via the bridge after background JPEG encode.
     *  Stores RequestId → CharacterId in PendingCameraRequests for VLM response routing.
     *  Must be called on the game thread. Called internally by UEstuaryCameraComponent. */
    UFUNCTION(BlueprintCallable, Category="Estuary|Camera",
              meta=(DisplayName="Emit Camera Image",
                    ToolTip="Emit a JPEG camera frame to the backend. Called internally by UEstuaryCameraComponent::SendCameraFrame."))
    void EmitCameraImage(const FEstuaryCameraImage& Payload);

    /** Emit user head floor xy for GestureSync dyadic conditioning (paper p_y).
     *  Call at the motion frame rate (~30 Hz) while a GestureSync session is
     *  active; the helper component UEstuaryUserPositionComponent does this
     *  automatically when attached to the bot character actor.
     *  Coordinates are in meters relative to the agent's reference frame
     *  (agent at origin, looking down +z; +x = agent's right). Silently
     *  dropped if the Socket.IO transport is not connected. */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Body",
              meta = (DisplayName = "Emit User Pos",
                      ToolTip = "Forward floor-projected user head xy to GestureSync for dyadic body-motion conditioning. Call at ~30 Hz while a GestureSync session is active."))
    void EmitUserPos(float X, float Y);

    /** Register a camera component for camera_capture event routing.
     *  The owning AEstuaryCharacter's CharacterId is used as the routing key.
     *  Called from UEstuaryCameraComponent::BeginPlay.
     *  Thread safety: check(IsInGameThread()) — call ONLY from BeginPlay. */
    void RegisterCameraComponent(UEstuaryCameraComponent* Component);

    /** Remove a camera component from the routing registry.
     *  Called from UEstuaryCameraComponent::EndPlay.
     *  Thread safety: check(IsInGameThread()) — call ONLY from EndPlay. */
    void UnregisterCameraComponent(UEstuaryCameraComponent* Component);

    // -----------------------------------------------------------------------
    // Capture source mode (CAM-07) — selects which camera the VLM "sees
    // through". Applied to all registered camera components (single-character
    // testbed has one). Governs the server-initiated camera_capture path.
    // -----------------------------------------------------------------------

    /** Flip every registered camera between Rendered Scene and Webcam.
     *  Returns the new mode. Intended for a UI toggle button. */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Camera",
              meta = (DisplayName = "Toggle Capture Source",
                      ToolTip = "Flip all registered cameras between Rendered Scene and Webcam. Returns the new mode."))
    EEstuaryCaptureSource ToggleCaptureSource();

    /** Set the capture source on every registered camera component (and new ones). */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Camera",
              meta = (DisplayName = "Set Capture Source (All)"))
    void SetCaptureSourceAll(EEstuaryCaptureSource NewSource);

    /** True when the current capture mode is Webcam. */
    UFUNCTION(BlueprintPure, Category = "Estuary|Camera",
              meta = (DisplayName = "Is Webcam Capture Mode"))
    bool IsWebcamCaptureMode() const { return DesiredCaptureSource == EEstuaryCaptureSource::Webcam; }

    /** UI-friendly label for the current capture source, e.g. "Camera: Webcam". */
    UFUNCTION(BlueprintPure, Category = "Estuary|Camera",
              meta = (DisplayName = "Get Capture Source Label",
                      ToolTip = "Returns 'Camera: Webcam' or 'Camera: Rendered Scene' for a UI label."))
    FText GetCaptureSourceLabel() const;

    // -----------------------------------------------------------------------
    // LiveKit bridge API — Phase 8 (LK-01, LK-02, LK-03).
    // Bridge is owned here; EstuaryLiveKit::StartupModule calls RegisterVoiceImpl.
    // -----------------------------------------------------------------------

    /**
     * Returns the current voice transport mode.
     * WebSocket — default; LiveKit — when EstuaryLiveKit module loaded and registered.
     *
     * Thread safety: always call on game thread.
     */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Estuary|LiveKit",
              meta = (DisplayName = "Get Voice Mode",
                      ToolTip = "Returns LiveKit when EstuaryLiveKit module is present and registered, otherwise WebSocket."))
    EEstuaryVoiceMode GetVoiceMode() const;

    /**
     * C++ accessor for the LiveKit bridge.
     * EstuaryLiveKit::StartupModule calls GetLiveKitBridge().RegisterVoiceImpl(impl).
     * Core components call GetLiveKitBridge().IsAvailable() before routing audio.
     */
    FEstuaryLiveKitBridge& GetLiveKitBridge()             { return LiveKitBridge; }
    const FEstuaryLiveKitBridge& GetLiveKitBridge() const { return LiveKitBridge; }

    /**
     * C++ accessor for the Live Link bridge.
     * EstuaryLiveLink::OnPostWorldInit calls GetLiveLinkBridge().RegisterPublisher(impl).
     * UEstuaryAnimationReceiver::TickComponent calls GetLiveLinkBridge().IsAvailable()
     * before publishing ARKit-52 weight frames.
     */
    FEstuaryLiveLinkBridge& GetLiveLinkBridge()             { return LiveLinkBridge; }
    const FEstuaryLiveLinkBridge& GetLiveLinkBridge() const { return LiveLinkBridge; }

    /**
     * Request a LiveKit token from the server. Emits the 'livekit_token' event (null payload)
     * per SDK_CONTRACT §voice_livekit. The server responds with a livekit_token event carrying
     * {token, url, room}; HandleLiveKitTokenReceived processes the response.
     *
     * No-op if not Connected or if LiveKit is not available (logs Warning).
     * No-op if already joined to a room (logs Verbose).
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|LiveKit",
              meta = (DisplayName = "Request LiveKit Token",
                      ToolTip = "Emit livekit_token request to the server. Server responds with token + url + room. Automatically called when voice mode starts and LiveKit is available."))
    void RequestLiveKitToken();

    /**
     * Leave the LiveKit room and notify the server via 'livekit_leave' (null payload).
     * Falls back to WebSocket voice mode after leaving.
     * No-op if not currently in a LiveKit room.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|LiveKit",
              meta = (DisplayName = "Leave LiveKit Room",
                      ToolTip = "Disconnect from the LiveKit room and revert to WebSocket voice mode."))
    void LeaveLiveKitRoom();

    /**
     * Returns the current session state. Use with OnStateChanged for UI/logic binding.
     * Valid to call in any state and from any UObject-safe context.
     */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Estuary|Transport",
              meta = (DisplayName = "Get Session State",
                      ToolTip = "Current EEstuarySessionState. Bind OnStateChanged to react to transitions."))
    EEstuarySessionState GetSessionState() const { return SessionState; }

    /**
     * Returns the most-recent session info from the gateway.
     * Fields are empty until OnSessionConnected has fired at least once.
     */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Estuary|Transport",
              meta = (DisplayName = "Get Session Info",
                      ToolTip = "Session metadata received from the gateway. Empty until OnSessionConnected fires."))
    FEstuarySessionInfo GetSessionInfo() const { return SessionInfo; }

    /**
     * Returns the CharacterId of the component that called StartVoiceSession most recently.
     * Empty when no voice session is active. Used by AEstuaryCharacter::EndPlay to gate
     * LeaveLiveKitRoom so unloading a non-owner character does not tear down another
     * character's active LK session in multi-character scenes.
     */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Estuary|Voice",
              meta = (DisplayName = "Get Active Voice Character Id",
                      ToolTip = "CharacterId currently owning the voice session, empty if none."))
    FString GetActiveVoiceCharacterId() const { return ActiveVoiceCharacterId; }

    /** True after a voice_timeout released the voice lane and before the next
     *  successful voice re-establishment (livekit_ready) or a fresh Connect().
     *  Poll from UI (e.g. on Tick) to show the auto-muted state without binding
     *  OnVoiceTimeout/OnVoiceReady. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Estuary|Voice",
              meta = (DisplayName = "Is Voice Suspended",
                      ToolTip = "True between a voice_timeout release and the next confirmed voice re-establishment (livekit_ready). Show the mic as auto-muted while true."))
    bool IsVoiceSuspended() const { return bVoiceSuspended; }

    // -----------------------------------------------------------------------
    // Blueprint-assignable delegates
    // -----------------------------------------------------------------------

    /** Fires when the session is established and ready for text / audio. */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (ToolTip = "Bind to receive session_info after a successful Connect."))
    FOnEstuarySessionConnected OnSessionConnected;

    /** Fires when the gateway rejects the auth payload. Session cannot proceed. */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (ToolTip = "Bind to handle authentication failures (wrong api_key, character_id, player_id)."))
    FOnEstuaryAuthError OnAuthError;

    /** Fires when a server-side policy cap prevents the session (e.g., concurrent_limit). */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (ToolTip = "Bind to handle server policy rejections (e.g., too many concurrent sessions)."))
    FOnEstuarySessionRejected OnSessionRejected;

    /** Fires on a non-terminal `error` event. The session stays open (no reconnect). */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (ToolTip = "Bind to handle non-terminal server errors (e.g., a request sent before authenticate). Session is not torn down."))
    FOnEstuaryError OnError;

    /** Fires on `quota_exceeded`. The gateway disconnects immediately after; the
     *  numeric fields (Current/Limit/Remaining/Tier) are 0/empty in the share-limit variant. */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (ToolTip = "Bind to handle quota / share-interaction-limit rejections. The server disconnects right after this fires."))
    FOnEstuaryQuotaExceeded OnQuotaExceeded;

    /** Fires on `memory_updated` — newly extracted memories pushed after a conversation ends. */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (ToolTip = "Bind to receive newly extracted memories after a conversation ends (memory_updated)."))
    FOnEstuaryMemoryUpdated OnMemoryUpdated;

    /** Fires on `session_timeout` — the server reaped an idle session. The gateway
     *  disconnects right after and the SDK does NOT auto-reconnect (reconnecting would
     *  re-establish billed voice resources with nobody talking, in a loop). Surface
     *  "Session ended due to inactivity" and require explicit user intent to Connect() again. */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (ToolTip = "Bind to handle server idle reaps (session_timeout). No auto-reconnect follows; call Connect() on explicit user intent."))
    FOnEstuarySessionTimeout OnSessionTimeout;

    /** Fires on `voice_timeout` — the server released voice resources after voice
     *  inactivity while the session stayed alive (e.g. the user kept texting). The socket
     *  STAYS CONNECTED and text continues; the SDK has already stopped the mic and
     *  released the voice transport. Recommended UX: show the mic as auto-muted and
     *  start a fresh voice session (StartListening) when the user unmutes. */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (ToolTip = "Bind to handle voice-idle releases (voice_timeout). Socket stays connected; show the mic as auto-muted and restart voice on user intent."))
    FOnEstuaryVoiceTimeout OnVoiceTimeout;

    /** Fires on `livekit_ready` — the server confirms the bot joined the room and the
     *  STT pipeline is armed. Speech before this can land on a cold pipeline and be
     *  lost (a LiveKit join takes ~1s, plus server STT warmup). Fires after EVERY join,
     *  including the auto-rejoin triggered by the first StartListening after a
     *  voice_timeout. Recommended UX: show "connecting voice…" from StartListening
     *  until this fires, then "voice ready". Never fires in pure-WebSocket voice mode
     *  (no server ack exists there). */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (ToolTip = "Fires when the server confirms the voice pipeline is live (livekit_ready). Show 'connecting voice' from StartListening until this fires — earlier speech can be lost to the cold join."))
    FOnEstuaryVoiceReady OnVoiceReady;

    /** Fires on every state transition. Useful for UI (connecting spinner, error banner). */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (ToolTip = "Fires on every EEstuarySessionState transition including Reconnecting and Error."))
    FOnEstuarySessionStateChanged OnStateChanged;

private:

    // -----------------------------------------------------------------------
    // Internal state — not BlueprintReadable; transitions go through SetState().
    // -----------------------------------------------------------------------

    /** Current session state. Set via SetState() which broadcasts OnStateChanged once. */
    EEstuarySessionState SessionState = EEstuarySessionState::Disconnected;

    /** Most-recent session info. Set in HandleSessionInfo(). */
    FEstuarySessionInfo SessionInfo;

    /** Cached Connect params for reconnect cycles. ApiKey stored here, NOT as a UPROPERTY
     *  so Blueprint inspector and SaveGame cannot discover it (threat T-02-11). */
    FString             CachedServerUrl;
    FEstuaryAuthPayload CachedAuth;

    /** Bridge — TSharedPtr because FEstuarySocketIOBridge uses TSharedFromThis.
     *  NOT UPROPERTY: FEstuarySocketIOBridge is non-UObject; Pitfall 3 inverse —
     *  do not UPROPERTY non-UObject types. */
    TSharedPtr<FEstuarySocketIOBridge> Bridge;

    /** Counts consecutive failed connect attempts; drives exponential backoff delay.
     *  Reset to 0 on successful session_info. */
    int32 ReconnectAttempts = 0;

    /** Handle for the one-shot FTSTicker reconnect callback. Reset after firing. */
    FTSTicker::FDelegateHandle ReconnectHandle;

    /** Set by HandleSessionTimeout: the SERVER ended this session (idle reap) and will
     *  disconnect the socket right after. Suppresses the auto-reconnect that
     *  HandleTransportClosed would otherwise schedule — reconnecting would re-auth and
     *  re-establish billed voice resources with nobody talking, in a loop
     *  (SDK_CONTRACT §session_timeout). Cleared only by an explicit Connect() (user intent).
     *  Guarded in BOTH HandleTransportClosed and TickReconnect: the session_timeout event
     *  and the transport close are marshaled to the game thread independently, so the
     *  close can land first and schedule a reconnect before this flag is set. */
    bool bServerEndedSession = false;

    /** True between a voice_timeout release and the next confirmed voice
     *  re-establishment (livekit_ready) or a fresh Connect(). Read via
     *  IsVoiceSuspended() for polling UIs. */
    bool bVoiceSuspended = false;

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /** Transition SessionState to NewState; broadcast OnStateChanged if different. */
    void SetState(EEstuarySessionState NewState);

    /** Wire TFunction sinks on Bridge to our handler methods.
     *  Must be called immediately after Bridge is created. */
    void BindBridgeSinks();

    /** Schedule a one-shot FTSTicker reconnect after exponential backoff delay.
     *  Increments ReconnectAttempts. */
    void ScheduleReconnect();

    /** Cancel any pending reconnect ticker. Safe to call if no ticker is pending. */
    void CancelReconnect();

    /** FTSTicker callback — actually fires the reconnect. Returns false (one-shot). */
    bool TickReconnect(float DeltaTime);

    // -----------------------------------------------------------------------
    // Bridge sink handlers — all run on the game thread (bridge guarantees this).
    // NOT UFUNCTION: these are TFunction callbacks, not DYNAMIC_MULTICAST receivers.
    // If refactored to AddDynamic, add UFUNCTION() (Pitfall 2).
    // -----------------------------------------------------------------------

    void HandleSessionInfo(const FEstuarySessionInfo& Info);
    void HandleAuthError(const FEstuaryAuthError& Err);
    void HandleSessionRejected(const FEstuarySessionRejected& Rej);

    /** Receive non-terminal `error` from bridge (game-thread guaranteed). Broadcasts OnError.
     *  Does NOT change session state and does NOT disconnect — the session stays open. */
    void HandleError(const FEstuaryError& Err);

    /** Receive `quota_exceeded` from bridge (game-thread guaranteed). Broadcasts OnQuotaExceeded.
     *  The gateway disconnects right after emitting; the subsequent transport close drives the
     *  state machine, so this handler only surfaces the typed payload (no SetState here). */
    void HandleQuotaExceeded(const FEstuaryQuotaExceeded& Quota);

    /** Receive `memory_updated` from bridge (game-thread guaranteed). Broadcasts OnMemoryUpdated. */
    void HandleMemoryUpdated(const FEstuaryMemoryUpdated& Memory);

    /** Receive `client_action` from bridge (game-thread guaranteed). Stamps CharacterId
     *  from session_info (not on the wire) and routes to the matching registered
     *  component — same routing as HandleBotResponse. Fire-on-arrival per
     *  SDK_CONTRACT §client_action (contract v1.9). */
    void HandleClientAction(const FEstuaryClientAction& Action);

    /** Receive `session_timeout` from bridge (game-thread guaranteed). Sets
     *  bServerEndedSession (reconnect suppression), releases local voice resources,
     *  and broadcasts OnSessionTimeout. Does NOT SetState — the transport close that
     *  immediately follows drives the state machine (same pattern as quota_exceeded). */
    void HandleSessionTimeout(const FEstuarySessionTimeout& Timeout);

    /** Receive `voice_timeout` from bridge (game-thread guaranteed). Releases local
     *  voice resources (mic off, LiveKit room disposed — it is already deleted
     *  server-side) and broadcasts OnVoiceTimeout. The socket stays connected; text
     *  continues. NEVER touches bServerEndedSession (no disconnect follows). */
    void HandleVoiceTimeout(const FEstuaryVoiceTimeout& Timeout);

    /** Release LOCAL voice resources without emitting anything to the server:
     *  stop mic capture on the active (or all) registered characters WITHOUT
     *  stop_voice, dispose the LiveKit room client-side WITHOUT livekit_leave
     *  (the server has already torn its side down), and clear
     *  ActiveVoiceCharacterId. Used by voice_timeout / session_timeout / any
     *  transport close of a voice-active session — the stale-voice-state bug
     *  class: an unprocessed server-side teardown leaves a hot mic and a
     *  "voice already active" client. Idempotent. */
    void ReleaseVoiceResourcesLocal(const TCHAR* Context);

    void HandleTransportConnected();
    void HandleTransportClosed(int32 CloseCode, const FString& Reason);
    void HandleTransportError(const FString& Message);

    // -----------------------------------------------------------------------
    // Character registry (TEXT-06). Key = CharacterId string from AEstuaryCharacter.
    // Values are TWeakObjectPtr to guard against components being GC'd between
    // registration and dispatch (Pitfall 3).
    // UPROPERTY ensures GC awareness; TWeakObjectPtr deduplication.
    // -----------------------------------------------------------------------
    UPROPERTY()
    TMap<FString, TWeakObjectPtr<UEstuaryCharacterComponent>> RegisteredCharacters;

    // -----------------------------------------------------------------------
    // Bridge sink handlers for Phase 3 events.
    // NOT UFUNCTION: these are TFunction callbacks assigned in BindBridgeSinks().
    // If re-wired to AddDynamic, mark UFUNCTION (Pitfall 2).
    // -----------------------------------------------------------------------

    /** Receive bot_response from bridge (game-thread guaranteed). Route to matching component. */
    void HandleBotResponse(const FEstuaryBotResponse& Response);

    /** Receive server interrupt ack from bridge (game-thread guaranteed). Route to matching component. */
    void HandleInterruptAck(const FEstuaryInterruptPayload& Payload);

    /** Receive bot_voice from bridge (game-thread guaranteed).
     *  bot_voice has no character_id wire field — broadcasts to ALL registered characters.
     *  Each UEstuaryAudioPlayerComponent filters by CurrentMessageId (Unity SDK pattern). */
    void HandleBotVoice(const FEstuaryBotVoice& Voice);

    /** Receive stt_response from bridge (game-thread guaranteed). Route to matching component.
     *  CharacterId routing: stt_response has no wire character_id — route to the component
     *  whose owner called StartVoiceSession (tracked via ActiveVoiceCharacterId). */
    void HandleSttResponse(const FEstuarySttResponse& Response);

    /** CharacterId of the component that called StartVoiceSession most recently.
     *  Used to route stt_response to the right component. Cleared on StopVoiceSession. */
    FString ActiveVoiceCharacterId;

    // -----------------------------------------------------------------------
    // Camera component registry (Phase 7 — CAM-03).
    // Key = CharacterId (same key as RegisteredCharacters).
    // TWeakObjectPtr guards against GC'd components (Pitfall 3).
    // UPROPERTY ensures GC awareness.
    // -----------------------------------------------------------------------
    UPROPERTY()
    TMap<FString, TWeakObjectPtr<UEstuaryCameraComponent>> RegisteredCameraComponents;

    /** Desired capture source (CAM-07), applied to all registered cameras and to
     *  new registrations. Authoritative mode behind the testbed toggle. */
    EEstuaryCaptureSource DesiredCaptureSource = EEstuaryCaptureSource::RenderedScene;

    // -----------------------------------------------------------------------
    // Pending camera request correlation table (Phase 7 — CAM-06).
    // Key   = request_id (from FEstuaryCameraImage.RequestId).
    // Value = character_id for routing the VLM response back.
    // Inserted in EmitCameraImage; removed on final bot_response match.
    // Cleared in Deinitialize to prevent stale cross-session entries (T-07-08).
    // -----------------------------------------------------------------------
    TMap<FString, FString> PendingCameraRequests;

    /** Route an inbound camera_capture event to all registered camera components.
     *  Called on the game thread by the OnCameraCaptureSink lambda in BindBridgeSinks. */
    void HandleCameraCapture(const FEstuaryCameraCapture& Capture);

    /** Receive bot_animation from bridge (game-thread guaranteed).
     *  Filters terminator frames (IsTerminator()==true) then broadcasts to all registered
     *  characters (no wire character_id — same broadcast-to-all pattern as HandleBotVoice).
     *  Each UEstuaryAnimationReceiver filters by message_id independently. */
    void HandleBotAnimation(const FEstuaryBotAnimation& Animation);

    /** Receive bot_pose from bridge (game-thread guaranteed) — Phase 4 GestureSync body animation.
     *  Filters terminator frames (IsTerminator()==true) then broadcasts to all registered
     *  characters (no wire character_id — same broadcast-to-all pattern as HandleBotAnimation).
     *  Each UEstuaryBodyAnimationReceiver filters by message_id independently. */
    void HandleBotPose(const FEstuaryBotPose& Pose);

    // -----------------------------------------------------------------------
    // LiveKit bridge — owned by the subsystem (Phase 8 — LK-01, LK-02).
    // EstuaryLiveKit::StartupModule calls LiveKitBridge.RegisterVoiceImpl(...).
    // EstuaryLiveKit::ShutdownModule calls LiveKitBridge.UnregisterVoiceImpl().
    // NOT UPROPERTY: FEstuaryLiveKitBridge is a plain C++ class.
    // -----------------------------------------------------------------------
    FEstuaryLiveKitBridge LiveKitBridge;

    // -----------------------------------------------------------------------
    // Live Link bridge — owned by the subsystem (Path B).
    // EstuaryLiveLink::OnPostWorldInit calls LiveLinkBridge.RegisterPublisher(...).
    // EstuaryLiveLink::ShutdownModule calls LiveLinkBridge.UnregisterPublisher().
    // NOT UPROPERTY: FEstuaryLiveLinkBridge is a plain C++ class.
    // -----------------------------------------------------------------------
    FEstuaryLiveLinkBridge LiveLinkBridge;

    /** Current voice transport mode. Starts as WebSocket; switches to LiveKit on
     *  successful JoinRoom; reverts to WebSocket on LeaveRoom or error. */
    EEstuaryVoiceMode CurrentVoiceMode = EEstuaryVoiceMode::WebSocket;

    // -----------------------------------------------------------------------
    // Bridge sink handlers for Phase 8 LiveKit events.
    // Wired in BindBridgeSinks() alongside existing sinks.
    // -----------------------------------------------------------------------

    /** Receive livekit_token event from the server {token, url, room}.
     *  Calls LiveKitBridge.GetVoice()->JoinRoom(Token, Url, Room).
     *  Per SDK_CONTRACT §voice_livekit Server->Client livekit_token payload.
     *  SECURITY (T-08-03): never log the Token string — JWT credential. */
    void HandleLiveKitTokenReceived(const FString& Token, const FString& Url, const FString& Room);

    /** Receive livekit_ready event from the server {room}.
     *  Emits livekit_join (null) back to server per SDK_CONTRACT.
     *  Fires after the LiveKit SDK reports successful room connection. */
    void HandleLiveKitReady(const FString& Room);

    /** Receive livekit_error event from the server {error}.
     *  Logs the error, leaves CurrentVoiceMode as WebSocket. */
    void HandleLiveKitError(const FString& Error);

    /** Called by the bridge's OnJoined delegate when the LiveKit room connects.
     *  Transitions CurrentVoiceMode to LiveKit. */
    void OnLiveKitVoiceJoined();

    /** Called by the bridge's OnLeft delegate when the LiveKit room disconnects.
     *  Transitions CurrentVoiceMode to WebSocket. Removes delegate bindings. */
    void OnLiveKitVoiceLeft();

    /** Called by the bridge's OnConnectionError delegate.
     *  Logs the error, transitions CurrentVoiceMode to WebSocket. Removes delegate bindings. */
    void OnLiveKitVoiceError(const FString& ErrorMessage);
};
