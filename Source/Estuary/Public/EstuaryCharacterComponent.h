// EstuaryCharacterComponent.h — Per-character conversation component.
// Pitfall 23: #pragma once, IWYU, no CoreMinimal.h.
// Pitfall 2: Every BlueprintAssignable delegate receiver must be UFUNCTION.
// Pitfall 7: Zero LiveKit references.

#pragma once

#include "CoreTypes.h"
#include "UObject/ObjectMacros.h"
#include "Components/ActorComponent.h"
#include "Containers/UnrealString.h"
#include "EstuaryTypes.h"

#include "EstuaryCharacterComponent.generated.h"

// ---------------------------------------------------------------------------
// Delegate declarations — USTRUCT payloads keep us under 8-param Blueprint cap (Pitfall 16).
// ---------------------------------------------------------------------------

/** Fires on each streaming bot_response chunk. bIsFinal=true on the last chunk. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuaryBotResponse,
    const FEstuaryBotResponse&, Response);

/** Fires when the server acknowledges an interrupt (or self-initiates one). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuaryInterruptAcked,
    const FEstuaryInterruptPayload&, Payload);

/** Fires for every inbound bot_voice chunk for this character.
 *  UEstuaryAudioPlayerComponent binds this automatically in BeginPlay via
 *  GetOwner()->FindComponentByClass<UEstuaryCharacterComponent>() auto-wiring. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuaryBotVoice,
    const FEstuaryBotVoice&, Voice);

/** Fires when the VLM result for a camera_image emission arrives (Phase 7 — CAM-06).
 *  Response.Description contains the assembled bot_response text for this vision turn.
 *  Response.bIsFinal is true on the final chunk. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuaryVisionResponse,
    const FEstuaryVisionResponse&, Response);

/** Fires when the server sends a camera_capture request to this character (Phase 7 — CAM-03).
 *  The camera component has already auto-triggered SendCameraFrame() if consent is true.
 *  Bind to track server-initiated capture events or to handle the no-consent case. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuaryCameraCaptureRequested,
    const FEstuaryCameraCapture&, Capture);

/** Fires on each partial or final stt_response from the server during a voice session.
 *  UEstuaryMicrophoneComponent binds this for auto-barge-in (Plan 05-02). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuarySttResponse,
    const FEstuarySttResponse&, Response);

/** Fires for each renderable bot_animation frame for this character (FrameIndex >= 0).
 *  The terminator frame (bIsFinal=true, Weights empty) is filtered out by
 *  UEstuarySubsystem before dispatch — callers never see it here.
 *  UEstuaryAnimationReceiver binds this automatically in BeginPlay. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuaryBotAnimation,
    const FEstuaryBotAnimation&, Animation);

/** Fires for each renderable bot_pose body-animation frame for this character (FrameIndex >= 0).
 *  The terminator frame (bIsFinal=true, Bones empty) is filtered out by UEstuarySubsystem
 *  before dispatch — callers never see it here. UEstuaryBodyAnimationReceiver binds this
 *  automatically in BeginPlay. (Phase 4 — GestureSync body animation.) */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuaryBotPose,
    const FEstuaryBotPose&, Pose);

/** Fires when the server delivers a typed in-world action call for this character
 *  (client_action, contract v1.9 — SCRUM-202). Exactly once per action call.
 *  Fire-on-arrival: trigger the action as soon as this fires; actions are not
 *  synchronized to TTS playback position. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEstuaryClientAction,
    const FEstuaryClientAction&, Action);

// ---------------------------------------------------------------------------
// UEstuaryCharacterComponent
// ---------------------------------------------------------------------------

/**
 * Actor component that manages per-character conversation state for an AEstuaryCharacter.
 *
 * Lifecycle:
 *   BeginPlay  — retrieves CharacterId from AEstuaryCharacter owner, registers with subsystem.
 *   EndPlay    — unregisters from subsystem.
 *
 * Blueprint usage:
 *   1. Bind OnBotResponse to receive streaming bot text chunks.
 *   2. Call SendText("Hello") to start a conversation turn.
 *   3. Call Interrupt() to abort a streaming response mid-flight.
 *   4. Call UpdatePreferences to change session-level behavior flags.
 *
 * Thread safety: all public methods must be called on the game thread.
 *   The subsystem guarantees HandleBotResponseFromSubsystem / HandleInterruptAckFromSubsystem
 *   are called on the game thread before dispatching to Blueprint.
 */
UCLASS(ClassGroup = (Estuary),
       meta = (BlueprintSpawnableComponent,
               DisplayName = "Estuary Character Component",
               ToolTip = "Add to AEstuaryCharacter to enable text conversation. Exposes SendText, Interrupt, and UpdatePreferences."))
class ESTUARY_API UEstuaryCharacterComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UEstuaryCharacterComponent();

    // -----------------------------------------------------------------------
    // Live Link subject naming (Path B)
    // -----------------------------------------------------------------------

    /** Optional override for the Live Link subject FName. When None (default),
     *  the Estuary character ID is used; when that too is empty, the owning
     *  actor's FName is used. Set this to disambiguate two characters that
     *  share a CharacterId (e.g., two instances of the same agent in the same
     *  scene). Resolution order: Override -> CharacterId -> OwnerActor->GetFName(). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Estuary|Animation",
              meta = (DisplayName = "Live Link Subject Override",
                      ToolTip = "Optional Live Link subject name override. None = use CharacterId."))
    FName LiveLinkSubjectOverride = NAME_None;

    /** Returns the FName the receiver passes to FEstuaryLiveLinkBridge::PublishFrame.
     *  Resolution: Override -> CharacterId -> OwnerActor->GetFName() -> "EstuaryDefault". */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Estuary|Animation",
              meta = (DisplayName = "Get Live Link Subject Name",
                      ToolTip = "Resolves the Live Link subject FName for this character."))
    FName GetLiveLinkSubjectName() const;

    // -----------------------------------------------------------------------
    // Blueprint-callable API (TEXT-02, TEXT-04, TEXT-05)
    // -----------------------------------------------------------------------

    /**
     * Send a text message to the bot.
     *
     * Generates a unique message_id (FGuid::NewGuid, Digits format — 32 hex chars, no dashes)
     * and emits the 'text' event via UEstuarySubsystem. The same message_id is echoed in
     * every subsequent bot_response chunk for this turn so callers can correlate them.
     *
     * @param Text   The user's message. Must be non-empty for the server to respond.
     * @return       The generated message_id. Use to correlate OnBotResponse chunks or
     *               to call Interrupt() targeting this specific message.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary",
              meta = (DisplayName = "Send Text",
                      ToolTip = "Send a text message to the bot. Returns the message_id for correlation."))
    FString SendText(const FString& Text);

    /**
     * Script the character to speak a prewritten line via TTS (skips the LLM).
     *
     * Emits the 'say_line' event via UEstuarySubsystem. The server auto-interrupts any
     * in-flight response and mints its OWN message_id, so — unlike SendText — there is no
     * client-side correlation id to return (the response arrives on OnBotResponse / OnBotVoice
     * with a server-generated message_id). The scripted line is saved to chat history as a
     * normal assistant message and the LLM/memory system treats it as something the character
     * naturally said.
     *
     * @param Text       The prewritten line to speak. Must be non-empty.
     * @param bTextOnly  When true, only bot_response is returned (no bot_voice audio). Default false.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary",
              meta = (DisplayName = "Say Line",
                      ToolTip = "Script the character to say prewritten text via TTS (no LLM). Auto-interrupts the in-flight response. Returns nothing — the server mints its own message_id."))
    void SayLine(const FString& Text, bool bTextOnly = false);

    /**
     * Interrupt the currently-streaming bot response.
     *
     * Emits 'client_interrupt' with CurrentMessageId (the most recently sent message).
     * After this call, OnBotResponse chunks for the interrupted message will stop arriving.
     * The server may also send an inbound 'interrupt' ack, which fires OnInterruptAcked.
     *
     * No-op if CurrentMessageId is empty (no active turn).
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary",
              meta = (DisplayName = "Interrupt",
                      ToolTip = "Interrupt the currently-streaming bot response. Fires OnInterruptAcked when server confirms."))
    void Interrupt();

    /**
     * Update session-level behavior preferences on the server.
     * Changes take effect for subsequent bot responses in this session.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary",
              meta = (DisplayName = "Update Preferences",
                      ToolTip = "Emit update_preferences to change session-level behavior flags."))
    void UpdatePreferences(const FEstuaryPreferences& Prefs);

    /**
     * Returns true if this component is currently registered with UEstuarySubsystem
     * and can receive bot_response routing. Becomes true after BeginPlay completes
     * registration and false after EndPlay unregisters.
     */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Estuary",
              meta = (DisplayName = "Is Registered",
                      ToolTip = "True if registered with UEstuarySubsystem and routing is active."))
    bool IsRegistered() const;

    // -----------------------------------------------------------------------
    // BlueprintAssignable delegates (TEXT-03)
    // -----------------------------------------------------------------------

    /**
     * Fires for every streaming bot_response chunk, including the final chunk
     * (Response.bIsFinal == true). Bind in Blueprint to build the response text
     * progressively or to detect completion.
     *
     * Note: chunks arrive per server LLM streaming. The first chunk may arrive within
     * 100-500 ms on a local server; subsequent chunks are faster.
     */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (DisplayName = "On Bot Response",
                      ToolTip = "Fires for each streaming bot_response chunk. Response.bIsFinal is true on the last chunk."))
    FOnEstuaryBotResponse OnBotResponse;

    /**
     * Fires when the server acknowledges a client_interrupt (or self-interrupts due to a
     * new user turn or backend policy). Inspect Payload.Reason for the cause.
     */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (DisplayName = "On Interrupt Acked",
                      ToolTip = "Fires when the server confirms an interrupt. Payload.Reason explains the cause."))
    FOnEstuaryInterruptAcked OnInterruptAcked;

    /**
     * Fires for each streaming bot_voice audio chunk for this character.
     * Audio is base64-encoded PCM16 mono. UEstuaryAudioPlayerComponent binds this
     * automatically in BeginPlay (auto-wiring pattern: sibling component on same actor).
     * Blueprint consumers can also bind directly to receive raw audio chunks.
     */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (DisplayName = "On Bot Voice",
                      ToolTip = "Fires for each streaming bot_voice chunk. Audio is base64 PCM16. UEstuaryAudioPlayerComponent auto-binds this on the same actor."))
    FOnEstuaryBotVoice OnBotVoice;

    /**
     * Fires for each partial or final stt_response result during a voice session.
     * Response.bIsFinal is true on the last result; server then triggers bot_response + bot_voice.
     * UEstuaryMicrophoneComponent binds this automatically for auto-barge-in (Plan 05-02).
     * Blueprint consumers can also bind directly to display live transcription.
     */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (DisplayName = "On STT Response",
                      ToolTip = "Fires for each STT partial/final result. Response.bIsFinal is true on the last result."))
    FOnEstuarySttResponse OnSttResponse;

    /**
     * Fires for each renderable ARKit-52 animation frame. FrameIndex >= 0 always
     * (terminator frames are filtered by UEstuarySubsystem before dispatch).
     * UEstuaryAnimationReceiver auto-binds this on the same actor in BeginPlay.
     */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (DisplayName = "On Bot Animation",
                      ToolTip = "Fires for each renderable ARKit-52 animation frame. UEstuaryAnimationReceiver auto-binds this on the same actor."))
    FOnEstuaryBotAnimation OnBotAnimation;

    /**
     * Fires for each renderable bot_pose body-animation frame (FrameIndex >= 0).
     * Terminator frames are filtered by UEstuarySubsystem before dispatch.
     * UEstuaryBodyAnimationReceiver auto-binds this on the same actor in BeginPlay.
     * (Phase 4 — GestureSync body animation.)
     */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (DisplayName = "On Bot Pose",
                      ToolTip = "Fires for each renderable bot_pose body-animation frame. UEstuaryBodyAnimationReceiver auto-binds this on the same actor."))
    FOnEstuaryBotPose OnBotPose;

    /**
     * Fires when a VLM response arrives for a prior camera_image emission (Phase 7 — CAM-06).
     * Response.bIsFinal is true on the final chunk. Response.Description accumulates
     * the bot_response text received so far for the matching request_id.
     */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (DisplayName = "On Vision Response",
                      ToolTip = "Fires when a VLM result arrives for a prior camera_image emission. Response.bIsFinal true on the last chunk."))
    FOnEstuaryVisionResponse OnVisionResponse;

    /**
     * Fires when the server sends a camera_capture request targeting this character (Phase 7 — CAM-03).
     * The camera component has already auto-triggered SendCameraFrame() if consent is true.
     * Bind to track server-initiated capture events or handle the no-consent case in Blueprint.
     */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (DisplayName = "On Camera Capture Requested",
                      ToolTip = "Fires when the server sends a camera_capture event for this character. Camera component auto-captures if consent is true."))
    FOnEstuaryCameraCaptureRequested OnCameraCaptureRequested;

    /**
     * Fires when the character performs a typed in-world action (client_action,
     * contract v1.9 — native LLM function calling, SCRUM-202). Exactly once per
     * action call; a turn may carry several (in call order). Fire-on-arrival:
     * trigger the action as soon as this fires — not synced to TTS playback.
     * Action.Arguments values are stringified (numbers invariant like "2.5",
     * booleans lowercase "true"/"false"); parameterless actions have an empty map.
     */
    UPROPERTY(BlueprintAssignable, Category = "Estuary|Events",
              meta = (DisplayName = "On Client Action",
                      ToolTip = "Fires when the character performs a typed in-world action (client_action). Exactly once per call, on arrival. Arguments are stringified key/value pairs."))
    FOnEstuaryClientAction OnClientAction;

    // -----------------------------------------------------------------------
    // Subsystem-internal dispatch (NOT Blueprint-visible; called by UEstuarySubsystem)
    // -----------------------------------------------------------------------

    /** Called by UEstuarySubsystem::HandleBotResponse on the game thread. Broadcasts OnBotResponse. */
    void HandleBotResponseFromSubsystem(const FEstuaryBotResponse& Response);

    /** Called by UEstuarySubsystem::HandleInterruptAck on the game thread. Broadcasts OnInterruptAcked. */
    void HandleInterruptAckFromSubsystem(const FEstuaryInterruptPayload& Payload);

    /** Called by UEstuarySubsystem::HandleBotVoice on the game thread. Broadcasts OnBotVoice. */
    void HandleBotVoiceFromSubsystem(const FEstuaryBotVoice& Voice);

    /** Called by UEstuarySubsystem::HandleSttResponse on the game thread. Broadcasts OnSttResponse. */
    void HandleSttResponseFromSubsystem(const FEstuarySttResponse& Response);

    /** Called by UEstuarySubsystem::HandleBotAnimation on the game thread.
     *  Terminator frames are pre-filtered by the subsystem; this always receives
     *  renderable frames (FrameIndex >= 0). Broadcasts OnBotAnimation. */
    void HandleBotAnimationFromSubsystem(const FEstuaryBotAnimation& Animation);

    /** Called by UEstuarySubsystem::HandleBotPose on the game thread.
     *  Terminator frames are pre-filtered by the subsystem; this always receives
     *  renderable frames (FrameIndex >= 0). Broadcasts OnBotPose.
     *  (Phase 4 — GestureSync body animation.) */
    void HandleBotPoseFromSubsystem(const FEstuaryBotPose& Pose);

    /**
     * Toggle ARKit-52 lipsync mode. When true:
     *   - enable_animation: true is included in the next authenticate payload.
     *   - AudioSampleRate is coerced to 16000 Hz (A2F gate — AUDIO-07 / ANIM-08).
     *   - A sibling UEstuaryAudioPlayerComponent::SetEnableAnimation(true) is called
     *     automatically if found on the same actor.
     * Must be called before Connect(); changes take effect on the next Connect.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary",
              meta = (DisplayName = "Set Enable Animation",
                      ToolTip = "Enable ARKit-52 lipsync. Coerces AudioSampleRate to 16000 Hz. Call before Connect()."))
    void SetEnableAnimation(bool bEnable);

    /**
     * Toggle bot_pose body-animation reception for this character (Phase 4 — GestureSync).
     * When true, the next authenticate payload (after the next Connect) will carry
     * enable_body_animation=true. The 16 kHz HuBERT gate is enforced server-side by
     * GestureSync's inference path — no client-side audio rate coercion is performed at
     * first pass (parity with how SetEnableAnimation/AudioRate are decoupled in
     * the v0.3 update). Must be called before Connect(); changes take effect on
     * the next Connect.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Body Animation",
              meta = (DisplayName = "Set Enable Body Animation",
                      ToolTip = "Toggle the bot_pose body-animation stream for this character. Takes effect on next reconnect (auth payload boundary)."))
    void SetEnableBodyAnimation(bool bEnable);

    /** Called by UEstuarySubsystem::HandleBotResponse when message_id matches a pending camera
     *  request_id. Broadcasts OnVisionResponse on the game thread. */
    void HandleVisionResponseFromSubsystem(const FEstuaryVisionResponse& Response);

    /** Called by UEstuaryCameraComponent::HandleCameraCaptureFromSubsystem to also broadcast
     *  the raw server request to Blueprint listeners. Broadcasts OnCameraCaptureRequested. */
    void HandleCameraCaptureRequestedFromSubsystem(const FEstuaryCameraCapture& Capture);

    /** Called by UEstuarySubsystem::HandleClientAction on the game thread.
     *  Broadcasts OnClientAction (fire-on-arrival, contract v1.9). */
    void HandleClientActionFromSubsystem(const FEstuaryClientAction& Action);

    /**
     * Returns the CharacterId inherited from the owning AEstuaryCharacter.
     * Used by UEstuarySubsystem::RegisterCharacter for registry keying.
     * Returns empty string if GetOwner() is null or has no CharacterId set.
     */
    FString GetCharacterId() const;

    /**
     * Returns the message_id of the most recently started turn (via SendText).
     * Empty when no turn is active. Used by UEstuaryMicrophoneComponent for auto-barge-in
     * to verify the bot has an active message_id before calling Interrupt().
     */
    FString GetCurrentMessageId() const { return CurrentMessageId; }

    // -----------------------------------------------------------------------
    // UActorComponent overrides
    // -----------------------------------------------------------------------

    /** Retrieves CharacterId from AEstuaryCharacter owner and registers with subsystem. */
    virtual void BeginPlay() override;

    /** Unregisters from subsystem. */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

    /** CharacterId cached from AEstuaryCharacter on BeginPlay. Keyed in the subsystem registry. */
    FString CachedCharacterId;

    /**
     * The message_id of the most recently started turn (via SendText).
     * Used by Interrupt() to target the in-flight message.
     * Empty when no turn is active.
     */
    FString CurrentMessageId;

    /**
     * True between a successful RegisterCharacter and the matching UnregisterCharacter.
     * Guards against double-registration and stale dispatch.
     */
    bool bIsRegistered = false;

    /** True when SetEnableAnimation(true) has been called. Forwarded to the subsystem
     *  at the next Connect() to include enable_animation in the auth payload. */
    bool bEnableAnimation = false;

    /** True when SetEnableBodyAnimation(true) has been called. Forwarded to the auth
     *  payload at the next Connect() to include enable_body_animation. (Phase 4 GestureSync.)
     *  Mirrors bEnableAnimation pattern — plain private bool, no UPROPERTY (UHT
     *  forbids BlueprintReadWrite on private members). BP access via the
     *  SetEnableBodyAnimation UFUNCTION. */
    bool bEnableBodyAnimation = false;
};
