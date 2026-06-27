// EstuaryAudioPlayerComponent.h — Streaming TTS audio player for Estuary bot_voice.
// Pitfall 23: #pragma once, IWYU, no CoreMinimal.h, no Engine.h.
// Pitfall 2:  HandleBotVoiceEvent / HandleInterruptAckedEvent are UFUNCTION() so
//             AddDynamic binding works with DYNAMIC_MULTICAST_DELEGATE.
// Pitfall 4:  200 ms pre-buffer gate — Play() is NOT called until IsPrimed().
// Pitfall 8:  iOS AVAudioSession shim called before first Play().
//
// Auto-wiring pattern:
//   BeginPlay calls GetOwner()->FindComponentByClass<UEstuaryCharacterComponent>()
//   and binds OnBotVoice + OnInterruptAcked automatically. If no sibling component
//   is found, logs a Warning (component is still usable when wired manually).

#pragma once

#include "Components/AudioComponent.h"
#include "EstuaryTypes.h"

#include "EstuaryAudioPlayerComponent.generated.h"

// Forward declarations — full headers included in .cpp only (Pitfall 21).
class UEstuaryCharacterComponent;
class UEstuarySubsystem;
class USoundWaveProcedural;

/**
 * UEstuaryAudioPlayerComponent — Streaming TTS playback component.
 *
 * Attach to the same actor as UEstuaryCharacterComponent for auto-wiring.
 * On BeginPlay, finds its sibling UEstuaryCharacterComponent and binds to
 * OnBotVoice and OnInterruptAcked automatically (no Blueprint wiring required).
 *
 * Audio pipeline:
 *   1. EnqueueBotVoice decodes base64 PCM16 and pushes int16_t samples into the ring buffer.
 *   2. Play() is deferred until IsPrimed() (200 ms of samples buffered — Pitfall 4 gate).
 *   3. OnGeneratePCMAudio on the audio thread drains samples lock-free.
 *   4. TickComponent detects tail drain (bFinalChunkReceived + GetAvailableSamples()==0)
 *      and emits audio_playback_complete via the subsystem.
 *   5. FlushAllForMessage (called on interrupt) applies silence ramp and stops playback
 *      WITHOUT emitting audio_playback_complete (interrupted turns are not "complete").
 *
 * Thread safety:
 *   All public methods must be called on the game thread.
 *   Ring buffer SPSC: game thread produces (EnqueueBotVoice), audio thread consumes.
 */
UCLASS(ClassGroup = (Estuary),
       meta = (BlueprintSpawnableComponent,
               DisplayName = "Estuary Audio Player Component",
               ToolTip = "Streaming TTS audio player. Attach alongside UEstuaryCharacterComponent for automatic bot_voice playback with 200ms pre-buffer and silence-ramp flush."))
class ESTUARY_API UEstuaryAudioPlayerComponent : public UAudioComponent
{
    GENERATED_BODY()

public:
    UEstuaryAudioPlayerComponent();

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------

    /** Decode base64 PCM16 from Voice.Audio and enqueue into the ring buffer.
     *  Filters chunks whose MessageId != CurrentMessageId (stale-message guard).
     *  Calls Play() when IsPrimed() (200 ms gate). Thread: game thread ONLY.
     *  Called automatically via HandleBotVoiceEvent bound to OnBotVoice in BeginPlay. */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Audio",
              meta = (DisplayName = "Enqueue Bot Voice",
                      ToolTip = "Decode and enqueue a bot_voice chunk. Called automatically by the auto-wired OnBotVoice binding."))
    void EnqueueBotVoice(const FEstuaryBotVoice& Voice);

    /** Enqueue a chunk of raw int16 PCM directly (no base64 decode).
     *  Called from the LiveKit voice impl after marshaling a remote AudioFrame to the GT.
     *  Includes a sample-rate probe: if RemoteSampleRate differs from CurrentStreamSampleRate,
     *  reinitialises the procedural sound wave at the new rate. Game thread ONLY. */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Audio",
              meta = (DisplayName = "Enqueue Raw PCM16",
                      ToolTip = "Push raw PCM16 mono samples directly into the streaming buffer (LiveKit fast path)."))
    void EnqueueRawPcm16(const TArray<uint8>& PcmBytes, int32 RemoteSampleRate);

    /** Flush ring buffer with silence ramp; stop playback; do NOT emit audio_playback_complete.
     *  Called on interrupt (client_interrupt was sent; server already knows).
     *  If MessageId is non-empty and does not match CurrentMessageId, this is a no-op. */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Audio",
              meta = (DisplayName = "Flush All For Message",
                      ToolTip = "Silence-ramp flush for an interrupted message. Does not emit audio_playback_complete."))
    void FlushAllForMessage(const FString& MessageId);

    /** Enable or disable animation mode. When true, coerces stream sample rate to 16000 Hz
     *  (A2F gate requirement — Pitfall 9 / AUDIO-07). Logs a warning if coercion occurs. */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Audio",
              meta = (DisplayName = "Set Enable Animation",
                      ToolTip = "Enable ARKit-52 lipsync mode. Coerces AudioSampleRate to 16000 Hz when true."))
    void SetEnableAnimation(bool bEnable);

    /**
     * Returns the current TTS audio playout position in seconds.
     * Derived from the number of PCM samples consumed by the audio thread
     * divided by the negotiated sample rate (CurrentStreamSampleRate).
     *
     * Used by UEstuaryAnimationReceiver as the authoritative time base for
     * audio-clock-driven lipsync (Pitfall 18 / ANIM-05).
     *
     * Returns 0.0f when no audio is playing or the procedural wave is null.
     * Thread safety: reads from UEstuarySoundWaveProcedural's atomic ReadIndex
     * (safe to call on the game thread).
     */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Estuary|Audio",
              meta = (DisplayName = "Get Playback Time Seconds",
                      ToolTip = "Current TTS playout position in seconds. Used by UEstuaryAnimationReceiver for lipsync timing."))
    float GetPlaybackTimeSeconds() const;

    /**
     * Returns the current audio playback position in seconds for lipsync synchronization.
     *
     * In WebSocket mode: derives from cumulative bytes consumed by the audio thread divided
     *                    by CurrentStreamSampleRate (same as GetPlaybackTimeSeconds).
     * In LiveKit mode:   reads the playout clock from IEstuaryLiveKitVoice::GetPlayoutClockSeconds(),
     *                    which is updated atomically by the SDK reader thread on each remote
     *                    AudioFrame (LK-05 SHIPPED).
     *
     * Called by UEstuaryAnimationReceiver::TickComponent for audio-clock-driven frame selection.
     * Dual-clock pattern — mirrors Unity SDK EstuaryAnimationReceiver dual-clock.
     *
     * Must be called on the game thread (UEstuaryAnimationReceiver ticks on game thread).
     * Returns 0.0f when not playing or when LK is unavailable.
     */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Estuary|Audio",
              meta = (DisplayName = "Get Current Audio Time",
                      ToolTip = "Current audio playout position in seconds, voice-mode aware. Use this instead of GetPlaybackTimeSeconds when LiveKit may be active."))
    float GetCurrentAudioTime() const;

    /** Smoothed amplitude-driven jawOpen weight (0..JAW_MAX where JAW_MAX = 0.6, matching
     *  web SDK JawRampLayer). Returns 0 when no recent envelope update (audio silent or stopped).
     *  Game thread only. Engine envelope follower marshals values from audio thread automatically. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Estuary|Animation",
              meta = (DisplayName = "Get Current Jaw Weight",
                      ToolTip = "Smoothed amplitude-driven jawOpen weight (0..0.6). Used by UEstuaryAnimationReceiver as the amplitude-jaw layer."))
    float GetCurrentJawWeight() const;

    /** Game-thread timestamp (FPlatformTime::Seconds units) of the last engine envelope
     *  follower callback. Diagnostic only — DO NOT use for end-of-utterance detection
     *  because the engine envelope follower fires every audio render block (including
     *  silence) so the timestamp stays fresh while the audio component is "playing"
     *  even after content drains. Use IsBotAudioActive() instead. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Estuary|Animation",
              meta = (DisplayName = "Get Last Envelope Update Time",
                      ToolTip = "Diagnostic only. Timestamp of last audio envelope follower callback. NOT a reliable end-of-utterance signal — use IsBotAudioActive()."))
    double GetLastEnvelopeUpdateTime() const;

    /** True while bot audio is actively being heard. Flips false when:
     *   (a) tail drains naturally (TickComponent detects bFinalChunkReceived + bytes==0
     *       and emits audio_playback_complete); or
     *   (b) FlushAllForMessage runs after an interrupt; or
     *   (c) a sample-rate change forces Stop()+ResetAudio().
     *  This is the canonical "is bot audio currently playing right now" signal — used
     *  by UEstuaryAnimationReceiver to ramp the face back to neutral on natural utterance
     *  end. Reflects bIsPlaying internal state. Game thread only. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Estuary|Animation",
              meta = (DisplayName = "Is Bot Audio Active",
                      ToolTip = "True while bot audio is actively being heard; false after tail-drain or interrupt. Canonical end-of-utterance signal."))
    bool IsBotAudioActive() const;

    // -----------------------------------------------------------------------
    // UActorComponent overrides
    // -----------------------------------------------------------------------

    /** Lazy-construct StreamingSound at runtime to avoid persisting it (and its
     *  inherited AssetImportData) into the BP CDO. */
    virtual void OnRegister() override;

    /** Find sibling UEstuaryCharacterComponent and bind OnBotVoice + OnInterruptAcked.
     *  Also caches the subsystem pointer and sets the initial stream sample rate. */
    virtual void BeginPlay() override;

    /** Poll tail-drain condition: when bFinalChunkReceived && GetAvailableSamples()==0,
     *  emit audio_playback_complete once and reset playback state. */
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

    /** Unbind OnBotVoice and OnInterruptAcked delegates to prevent dangling callbacks. */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    // -----------------------------------------------------------------------
    // UFUNCTION delegate receivers (Pitfall 2: MUST be UFUNCTION for AddDynamic)
    // -----------------------------------------------------------------------

    /** Bound to UEstuaryCharacterComponent::OnBotVoice in BeginPlay. Calls EnqueueBotVoice. */
    UFUNCTION()
    void HandleBotVoiceEvent(const FEstuaryBotVoice& Voice);

    /** Bound to UEstuaryCharacterComponent::OnInterruptAcked in BeginPlay. Calls FlushAllForMessage. */
    UFUNCTION()
    void HandleInterruptAckedEvent(const FEstuaryInterruptPayload& Payload);

    /** Bound to OnAudioSingleEnvelopeValueNative in BeginPlay. Game thread (engine marshals).
     *  Note: NOT a UFUNCTION because OnAudioSingleEnvelopeValueNative uses .AddUObject (not AddDynamic). */
    void HandleEnvelopeValue(const class UAudioComponent* InComponent,
                             const class USoundWave* InWave,
                             const float InValue);

    // -----------------------------------------------------------------------
    // Private state
    // -----------------------------------------------------------------------

    /** Owned vanilla USoundWaveProcedural — its internal queue holds the streaming PCM.
     *  Created in constructor. UPROPERTY for GC awareness (Pitfall 3).
     *  We feed it via Wave->QueueAudio() and read its drain via Wave->GetAvailableAudioByteCount().
     *  Path B: NOT a subclass — sidesteps UE 5.6+ IAudioProxyDataFactory pure-virtual problem
     *  (Convai pattern: own as a member, don't inherit).
     *  Transient + DuplicateTransient: USoundWaveProcedural inherits AssetImportData from
     *  USoundBase which makes the BP CDO save fail with "Illegal reference to private object"
     *  on cross-package serialization. The runtime sound is constructed fresh on every
     *  PostInitProperties / SetupSoundData; never persist it. */
    UPROPERTY(Transient, DuplicateTransient)
    TObjectPtr<USoundWaveProcedural> StreamingSound = nullptr;

    /** Cumulative bytes ever passed to StreamingSound->QueueAudio(). Game-thread tracked.
     *  Combined with GetAvailableAudioByteCount() this gives us "bytes consumed by audio
     *  thread", which divided by sample size + sample rate yields the playout-position
     *  audio clock (Pitfall 18, ANIM-05). */
    int64 TotalBytesQueued = 0;

    /** message_id of the currently-playing audio turn. Empty when idle. */
    FString CurrentMessageId;

    /** Index of the last successfully enqueued chunk (for diagnostics). -1 = none. */
    int32 LastEnqueuedChunkIndex = -1;

    /** True after the bIsFinal=true chunk for CurrentMessageId has been enqueued.
     *  TickComponent watches this flag combined with GetAvailableSamples()==0 for drain detection. */
    bool bFinalChunkReceived = false;

    /** True while audio is playing (Play() was called and playback has not yet stopped). */
    bool bIsPlaying = false;

    /** Game-thread timestamp (FPlatformTime::Seconds) of the most recent EnqueueBotVoice
     *  or EnqueueRawPcm16 call. Used by TickComponent to detect "bot audio source has
     *  stopped pushing PCM" for WebSocket underrun / WS end-of-stream. 0.0 = no enqueue
     *  yet. Reset to 0.0 when bIsPlaying flips false so a fresh utterance restarts. */
    double LastEnqueueTime = 0.0;

    /** Game-thread timestamp (FPlatformTime::Seconds) of the most recent envelope-follower
     *  callback whose value exceeded the silence threshold. Used by TickComponent to
     *  detect "bot audio is silent right now" for LiveKit voice mode — LK pushes silence
     *  frames continuously when the bot isn't speaking (WebRTC tracks are continuous),
     *  so LastEnqueueTime stays fresh but the envelope value goes near zero. 0.0 = no
     *  non-silent envelope value seen yet. Updated in HandleEnvelopeValue. */
    double LastNonSilentEnvelopeTime = 0.0;

    /** True if SetEnableAnimation(true) has been called. Coerces sample rate to 16000. */
    bool bEnableAnimationActive = false;

    /** Current negotiated stream sample rate (updated by SetEnableAnimation and SetStreamSampleRate). */
    int32 CurrentStreamSampleRate = 16000;

    /** iOS AVAudioSession flag: EnsureVoiceSession() has already been called this session.
     *  Prevents redundant session category changes (T-04-04-03 mitigation). */
    bool bIOSSessionActive = false;

    /** Latest smoothed envelope value from UAudioComponent::OnAudioSingleEnvelopeValueNative
     *  (audio thread → game thread marshaled by the engine via FAudioThread::RunCommandOnGameThread). */
    float CurrentEnvelopeValue = 0.0f;

    /** FPlatformTime::Seconds when CurrentEnvelopeValue was last updated by the engine.
     *  Used to detect audio gaps (no callback for >100ms means audio stopped or underran). */
    double LastEnvelopeUpdateTime = 0.0;

    /** Cached subsystem pointer (set in BeginPlay, TWeakObjectPtr for GC safety — Pitfall 3). */
    TWeakObjectPtr<UEstuarySubsystem> WeakSubsystem;

    // -----------------------------------------------------------------------
    // AEC far-end reference tap (Pitfall 27)
    // -----------------------------------------------------------------------

    /** Mirror of every PCM byte queued to StreamingSound, released to
     *  IEstuaryLiveKitVoice::PushPlaybackPcm at audio-thread consumption pace by
     *  PumpAecReference(). This gives the LiveKit APM a far-end (speaker) echo
     *  reference whose timing matches actual rendering — enqueue time is wrong by the
     *  200 ms pre-buffer plus the variable ring depth. Mirrors at most what the
     *  procedural wave itself is buffering. Game thread only. */
    TArray<uint8> AecReferenceFifo;

    /** Bytes consumed by the audio thread that have already been released from
     *  AecReferenceFifo to the AEC reverse stream — baseline for the per-tick delta.
     *  Reset alongside TotalBytesQueued whenever the wave is ResetAudio()'d. */
    int64 AecConsumedBaseline = 0;

    /** UEstuaryDeveloperSettings::bLiveKitEchoCancellation cached once in BeginPlay so
     *  the gate stays session-constant (a live settings read could flip mid-stream and
     *  desynchronise the mirror). */
    bool bAecTapEnabled = true;

    /** True when AEC is enabled in settings AND a LiveKit voice impl is registered
     *  (the only AEC consumer). Both inputs are session-constant, so gating the append
     *  and the pump on this cannot desynchronise the mirror. */
    bool IsAecTapAvailable() const;

    /** Mirror freshly queued PCM bytes for the AEC reference feed. Call immediately
     *  after every StreamingSound->QueueAudio(), with the byte count actually queued. */
    void AppendAecReference(const uint8* Data, int32 NumBytes);

    /** Release (TotalBytesQueued - GetAvailableAudioByteCount()) - baseline bytes from
     *  the FIFO to PushPlaybackPcm. Called once per TickComponent. */
    void PumpAecReference();

    /** Clear the FIFO and baseline. Call wherever StreamingSound->ResetAudio() +
     *  TotalBytesQueued = 0 happen (flush, sample-rate reinit, BeginPlay). */
    void ResetAecReference();
};
