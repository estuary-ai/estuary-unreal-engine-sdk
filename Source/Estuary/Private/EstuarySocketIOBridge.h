// EstuarySocketIOBridge.h — Internal adapter hiding getnamo SocketIOClient-Unreal. Private/ only.
//
// This header MUST NOT be included from any Source/Estuary/Public/ header.
// getnamo types are forward-declared here and fully included only in the .cpp.
// Pitfall 21: keep internals internal — no getnamo symbols on the public surface.
// Pitfall 7:  no getnamo symbols in the core module's public headers.
//
// Pitfall 23: #pragma once, no #ifndef guards, no CoreMinimal.h.

#pragma once

#include "CoreTypes.h"
#include "Containers/UnrealString.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "EstuaryTypes.h"

/**
 * FEstuarySocketIOBridge — Thin private adapter that wraps getnamo/SocketIOClient-Unreal v2.11.0.
 *
 * Responsibilities:
 *  - Connect to the Estuary gateway's /sdk namespace over WebSocket-only transport.
 *  - Emit the `authenticate` event immediately on transport open.
 *  - Parse inbound JSON events into typed USTRUCTs.
 *  - Dispatch ALL events to TFunction sinks on the game thread.
 *  - Disable getnamo's internal auto-reconnect; reconnect policy lives in UEstuarySubsystem.
 *
 * Lifetime: non-UObject, managed by UEstuarySubsystem via TSharedPtr.
 * Thread safety: Connect/Disconnect/IsConnected must be called on the game thread.
 *               Inbound getnamo callbacks run on getnamo's worker thread; bridge
 *               marshals them to the game thread via AsyncTask before invoking sinks.
 */
class FEstuarySocketIOBridge
    : public TSharedFromThis<FEstuarySocketIOBridge, ESPMode::ThreadSafe>
    , public FNoncopyable
{
public:
    FEstuarySocketIOBridge();
    ~FEstuarySocketIOBridge();

    // -----------------------------------------------------------------------
    // Connection API — must be called on the game thread.
    // -----------------------------------------------------------------------

    /** Open a Socket.IO v4 WebSocket connection to ServerUrl on the /sdk namespace,
     *  then emit the `authenticate` event with Auth on transport open. */
    void Connect(const FString& ServerUrl, const FEstuaryAuthPayload& Auth);

    /** Gracefully close the socket. Does NOT fire OnTransportClosed.
     *  Sets bShuttingDown so any in-flight marshaled dispatches become no-ops. */
    void Disconnect();

    /** True if the underlying socket considers itself connected. Thread-safe read. */
    bool IsConnected() const;

    // -----------------------------------------------------------------------
    // Text-chat emit API (Plan 03-02; REQ TEXT-02, TEXT-04, TEXT-05).
    // All must be called on the game thread; no-op Warning log if not connected.
    // -----------------------------------------------------------------------

    /** Emit `text` on /sdk with {text, textOnly} JSON. CharacterId / MessageId
     *  from the struct are NOT transmitted (not contract fields) but are used by the
     *  subsystem for correlation on inbound bot_response. */
    void EmitUserText(const FEstuaryUserText& Payload);

    /** Emit `client_interrupt` on /sdk with {message_id?}. Omits the key entirely when
     *  Payload.MessageId is empty (server interprets absent key as "current in-flight"). */
    void EmitInterrupt(const FEstuaryInterruptPayload& Payload);

    /** Emit `update_preferences` on /sdk. Serializes `bEnableVisionAcknowledgment` as
     *  camelCase key `enableVisionAcknowledgment` (per SDK_CONTRACT §preferences) plus
     *  verbatim keys/values from `ExtraPreferences` (forward-compat bag). */
    void EmitUpdatePreferences(const FEstuaryPreferences& Payload);

    /** Emit `say_line` on /sdk with {text, text_only} JSON. Scripts the character to
     *  speak prewritten text (TTS only, skips the LLM). Note SNAKE_case `text_only`
     *  here (per SDK_CONTRACT §say_line) — distinct from `text`'s camelCase `textOnly`.
     *  The server auto-interrupts the in-flight response and mints its OWN message_id,
     *  so there is no client-side correlation id (unlike EmitUserText). Response flows
     *  back through the existing bot_response / bot_voice path. */
    void EmitSayLine(const FString& Text, bool bTextOnly);

    // -----------------------------------------------------------------------
    // Event sinks — assign BEFORE calling Connect().
    // All sinks fire on the game thread (bridge marshals internally).
    // -----------------------------------------------------------------------

    /** Fired after the WebSocket transport is open and `authenticate` has been emitted.
     *  Note: session is NOT yet live — wait for OnSessionInfo. */
    TFunction<void()>                                       OnTransportConnected;

    /** Fired on transport close. CloseCode 1000 = clean; 1011 = gateway sticky-session
     *  failure; 1006 = abnormal closure (network drop). */
    TFunction<void(int32 CloseCode, const FString& Reason)> OnTransportClosed;

    /** Fired on a transport-level error (connection failure, protocol error). */
    TFunction<void(const FString& Message)>                 OnTransportError;

    /** Fired when `session_info` is received — session is live. */
    TFunction<void(const FEstuarySessionInfo&)>             OnSessionInfo;

    /** Fired when `auth_error` is received — credentials rejected. */
    TFunction<void(const FEstuaryAuthError&)>               OnAuthError;

    /** Fired when `session_rejected` is received — policy cap exceeded. */
    TFunction<void(const FEstuarySessionRejected&)>         OnSessionRejected;

    /** Fired when `error` is received — non-terminal server error. Session stays open. */
    TFunction<void(const FEstuaryError&)>                   OnError;

    /** Fired when `quota_exceeded` is received — quota / share-interaction-limit hit.
     *  The gateway disconnects immediately after; numeric fields may be absent
     *  (share-limit variant), in which case they default to 0 / empty. */
    TFunction<void(const FEstuaryQuotaExceeded&)>           OnQuotaExceeded;

    /** Fired when `memory_updated` is received — newly extracted memories pushed
     *  after a conversation ends. NewMemories items use camelCase wire keys. */
    TFunction<void(const FEstuaryMemoryUpdated&)>           OnMemoryUpdated;

    // -----------------------------------------------------------------------
    // Text-chat inbound sinks (Plan 03-02).
    // -----------------------------------------------------------------------

    /** Fired for every `bot_response` chunk (streaming). is_final=true on final chunk. */
    TFunction<void(const FEstuaryBotResponse&)>             OnBotResponse;

    /** Fired for inbound `interrupt` events (server acks client_interrupt OR self-initiates). */
    TFunction<void(const FEstuaryInterruptPayload&)>        OnInterruptAck;

    // -----------------------------------------------------------------------
    // Audio inbound sink (AUDIO-01, AUDIO-03).
    // -----------------------------------------------------------------------

    /** Fired for every inbound `bot_voice` chunk. Fires on the game thread.
     *  Audio field is raw base64 PCM16; decode in the consumer (UEstuaryAudioPlayerComponent). */
    TFunction<void(const FEstuaryBotVoice&)>                OnBotVoiceSink;

    // -----------------------------------------------------------------------
    // Audio emit API (AUDIO-05).
    // -----------------------------------------------------------------------

    /** Emit `audio_playback_complete` on /sdk with {message_id}.
     *  Call when the tail of a message's audio drains from the ring buffer.
     *  Per SDK_CONTRACT §audio_playback_tracking: message_id is optional —
     *  emit empty string if the message_id is unknown. */
    void EmitAudioPlaybackComplete(const FEstuaryAudioPlaybackComplete& Payload);

    // -----------------------------------------------------------------------
    // Voice/STT emitters (Phase 5 — MIC-03).
    // Must be called on the game thread. Bridge asserts this internally.
    // -----------------------------------------------------------------------

    /** Emit 'start_voice' with a null payload. Call before streaming audio chunks. */
    void EmitStartVoice();

    /** Emit 'stream_audio' with base64-encoded PCM16 chunk. Payload.Audio must be non-empty. */
    void EmitStreamAudio(const FEstuaryStreamAudio& Payload);

    /** Emit 'stop_voice' with a null payload. Call after the last audio chunk. */
    void EmitStopVoice();

    // -----------------------------------------------------------------------
    // Voice/STT inbound sink (Phase 5 — MIC-04).
    // -----------------------------------------------------------------------

    /** Sink — called on the game thread when stt_response arrives from the server.
     *  Assigned in UEstuarySubsystem::BindBridgeSinks(). */
    TFunction<void(const FEstuarySttResponse&)> OnSttResponseSink;

    // -----------------------------------------------------------------------
    // Camera / VLM emit API (Phase 7 — CAM-01, CAM-02).
    // Must be called on the game thread.
    // T-07-02: Bridge is transport-only — never logs ImageJpegBase64 content.
    // -----------------------------------------------------------------------

    /** Emit `camera_image` on /sdk with SDK_CONTRACT wire keys:
     *  "image", "mime_type", "request_id" (omitted if empty),
     *  "text" (omitted if empty), "sample_rate" (omitted if 0).
     *  Called by UEstuaryCameraComponent after background JPEG encode completes. */
    void EmitCameraImage(const FEstuaryCameraImage& Payload);

    /** Emit `bind_pose` on /sdk with the pre-built JSON payload.
     *  Optional per-character bind-pose handshake — the body receiver currently
     *  relies on the server's identity-bind fallback and does not emit one.
     *  Payload = TSharedRef<FJsonObject> with a single "bones" object field.
     *  Requires "Dom/JsonObject.h" in the including .cpp (already included there). */
    void EmitBindPose(const TSharedRef<FJsonObject>& Payload);

    /** Emit `user_pos` on /sdk with the floor-projected user head xy.
     *  GestureSync dyadic conditioning (paper p_y) — required for spatially-aware
     *  body motion. X/Y are in meters, agent's reference frame at session
     *  start (agent at origin, looking down +z; +x is the agent's right).
     *  High-frequency event (~30 Hz); silently dropped when not connected. */
    void EmitUserPos(float X, float Y);

    // -----------------------------------------------------------------------
    // Camera / VLM inbound sinks (Phase 7 — CAM-03, CAM-06).
    // Assigned in UEstuarySubsystem::BindBridgeSinks().
    // Both fire on the game thread (bridge marshals internally).
    // -----------------------------------------------------------------------

    /** Sink — called on the game thread when `camera_capture` arrives from the server.
     *  The server proactively requests an image (e.g., vision intent detected in STT).
     *  Assigned in UEstuarySubsystem::BindBridgeSinks().
     *  T-07-01: All parsed fields null-checked; empty defaults for missing optional keys. */
    TFunction<void(const FEstuaryCameraCapture&)> OnCameraCaptureSink;

    // OnVisionResponseSink: called by UEstuarySubsystem when it detects a
    // bot_response final chunk whose message_id matches a pending camera request_id.
    // Not a direct Socket.IO subscription — VLM results flow via bot_response.
    // The bridge does NOT subscribe to a "vision_response" socket event.
    /** Sink — called by UEstuarySubsystem (not the bridge) when a final bot_response
     *  chunk matches a pending camera request_id. Assembled client-side into
     *  FEstuaryVisionResponse and broadcast on UEstuaryCharacterComponent::OnVisionResponse. */
    TFunction<void(const FEstuaryVisionResponse&)> OnVisionResponseSink;

    // -----------------------------------------------------------------------
    // Animation inbound sink (Phase 6 — ANIM-08).
    // Assigned in UEstuarySubsystem::BindBridgeSinks().
    // Fires on the game thread (bridge marshals internally).
    // -----------------------------------------------------------------------

    /** Sink — called on the game thread when `bot_animation` arrives from the server.
     *  Each event carries one ARKit-52 blendshape frame (or the terminator frame).
     *  Assigned in UEstuarySubsystem::BindBridgeSinks().
     *  The subsystem filters terminator frames before broadcasting to character components. */
    TFunction<void(const FEstuaryBotAnimation&)> OnBotAnimationSink;

    /** Sink — called on the game thread when `bot_pose` arrives from the server.
     *  Each event carries one GestureSync-driven body-animation frame (or the terminator frame).
     *  Assigned in UEstuarySubsystem::BindBridgeSinks() (Phase 4 — GestureSync body animation).
     *  The subsystem filters terminator frames before broadcasting to character components. */
    TFunction<void(const FEstuaryBotPose&)> OnBotPoseSink;

    // -----------------------------------------------------------------------
    // LiveKit server-to-client event sinks (Phase 8 — LK-03, LK-04).
    // Assigned in UEstuarySubsystem::BindBridgeSinks().
    // All fire on the game thread (bridge marshals internally).
    // SDK_CONTRACT §voice_livekit Server→Client events.
    // -----------------------------------------------------------------------

    /** Sink — called when `livekit_token` arrives from server: {token, url, room}.
     *  Subsystem calls GetVoice()->JoinRoom(Token, Url, Room) to connect the room.
     *  SECURITY (T-08-03): Token is JWT — never log its value. */
    TFunction<void(const FString& Token, const FString& Url, const FString& Room)> OnLiveKitTokenSink;

    /** Sink — called when `livekit_ready` arrives from server: {room}.
     *  Subsystem responds by emitting livekit_join (null) back to server. */
    TFunction<void(const FString& Room)> OnLiveKitReadySink;

    /** Sink — called when `livekit_error` arrives from server: {error}.
     *  Subsystem logs the error and stays on WebSocket voice mode. */
    TFunction<void(const FString& ErrorMsg)> OnLiveKitErrorSink;

    // -----------------------------------------------------------------------
    // LiveKit client-to-server emit API (Phase 8 — LK-03, LK-04).
    // Must be called on the game thread.
    // SDK_CONTRACT §voice_livekit Client→Server events (all null payload).
    // -----------------------------------------------------------------------

    /** Emit 'livekit_token' (null payload) to request a LiveKit token from the server.
     *  Server responds asynchronously with a livekit_token event carrying {token, url, room}. */
    void EmitLiveKitTokenRequest();

    /** Emit 'livekit_join' (null payload) to notify the server the client joined the room.
     *  Called after OnLiveKitReadySink fires and the room connection is established. */
    void EmitLiveKitJoin();

    /** Emit 'livekit_leave' (null payload) to notify the server the client is leaving.
     *  Called from UEstuarySubsystem::LeaveLiveKitRoom before tearing down the room. */
    void EmitLiveKitLeave();

private:
    // Pimpl — fully hides FSocketIONative and all other getnamo types from this header.
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};
