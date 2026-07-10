// EstuaryMicrophoneComponent.h — Cross-platform mic capture using Runtime Audio Importer.
// Pitfall 23: #pragma once, IWYU, no CoreMinimal.h.
// Pitfall 8 / Anti-Pattern 5: bIsMuted and bIsSuppressed are INDEPENDENT flags.
// Threading: capture thread only Enqueues to AudioQueue (SPSC). Game thread Dequeues in Tick.
//            No UObject method call from the capture thread — ever.

#pragma once

#include "CoreTypes.h"
#include "UObject/ObjectMacros.h"
#include "Components/ActorComponent.h"
#include "Containers/UnrealString.h"
#include "Containers/Queue.h"
#include "Templates/PimplPtr.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"
#include "EstuaryTypes.h"

#include "EstuaryMicrophoneComponent.generated.h"

// Forward declarations — never include Private headers or optional-module headers here.
class UEstuarySubsystem;
class UEstuaryCharacterComponent;

namespace Audio { class FAudioCapture; }
class FEstuaryAudioCaptureSession; // PIMPL defined in EstuaryMicrophoneComponent.cpp

/**
 * Cross-platform microphone capture component for Estuary voice input.
 *
 * Uses Runtime Audio Importer (UCapturableSoundWave) — NOT FVoiceModule or UAudioCaptureComponent,
 * which are Windows-only. This component works on Win64, Mac, iOS, and Android.
 *
 * Required: Runtime Audio Importer plugin (Fab) must be installed in the project. See README.
 *
 * Threading:
 *   RAI capture callback fires on a worker thread. The callback ONLY enqueues PCM16 chunks into
 *   AudioQueue (an SPSC TQueue). TickComponent (game thread) drains the queue and emits
 *   stream_audio frames. No UObject method is ever touched from the capture thread.
 *
 * Mute vs Suppress:
 *   bIsMuted — user intent. Set via SetMuted(). Never auto-changed by the SDK.
 *   bIsSuppressed — auto echo guard. Set by the consuming game code while the bot is speaking
 *                   (call SetSuppressed(true) when bot audio starts; SetSuppressed(false) when done).
 *   Audio only emits when BOTH are false.
 */
UCLASS(ClassGroup = (Estuary),
       meta = (BlueprintSpawnableComponent,
               DisplayName = "Estuary Microphone Component",
               ToolTip = "Add to AEstuaryCharacter to enable cross-platform voice input via Runtime Audio Importer."))
class ESTUARY_API UEstuaryMicrophoneComponent : public UActorComponent
{
    GENERATED_BODY()

public:

    UEstuaryMicrophoneComponent();

    // -----------------------------------------------------------------------
    // UActorComponent overrides
    // -----------------------------------------------------------------------

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                               FActorComponentTickFunction* ThisTickFunction) override;

    // -----------------------------------------------------------------------
    // Blueprint-callable capture control (MIC-01, MIC-03)
    // -----------------------------------------------------------------------

    /**
     * Begin audio capture and emit 'start_voice' to the server.
     * No-op if already capturing or if the subsystem is not connected.
     * Requests microphone permission on iOS/Android before starting (non-blocking;
     * see UEstuaryPermissions::RequestMicrophone).
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Voice",
              meta = (DisplayName = "Start Listening",
                      ToolTip = "Begin mic capture and emit start_voice. Requires Subsystem to be Connected."))
    void StartListening();

    /**
     * Stop audio capture and emit 'stop_voice' to the server.
     * No-op if not currently capturing.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Voice",
              meta = (DisplayName = "Stop Listening",
                      ToolTip = "Stop mic capture and emit stop_voice."))
    void StopListening();

    /**
     * Stop capture WITHOUT emitting stop_voice or flushing queued audio to the
     * server. For when the SERVER has already released the voice session
     * (voice_timeout / session_timeout / transport close) — emitting into the
     * closed stream is the stale-voice-state bug class (hot mic + a client that
     * still thinks voice is active). Queued chunks are discarded. Safe to call
     * when not capturing (no-op). Called by
     * UEstuarySubsystem::ReleaseVoiceResourcesLocal; apps normally never need it.
     */
    void StopCaptureLocal();

    /**
     * Set the user-initiated mute flag.
     * When true, captured audio is DROPPED (not emitted) but capture continues.
     * This flag is never auto-toggled by the SDK — it reflects user intent only.
     * (Anti-Pattern 5 from ARCHITECTURE.md: do NOT conflate with bIsSuppressed.)
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Voice",
              meta = (DisplayName = "Set Muted",
                      ToolTip = "Mute (true) or unmute (false) the user mic. Does not affect bIsSuppressed."))
    void SetMuted(bool bMuted);

    /**
     * Set the auto-suppression flag (echo guard while bot is speaking).
     * When true, captured audio is DROPPED to prevent echo feedback.
     * Call SetSuppressed(true) when bot TTS starts; SetSuppressed(false) when bot TTS ends.
     * Never call this to implement user mute — use SetMuted for that.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Voice",
              meta = (DisplayName = "Set Suppressed",
                      ToolTip = "Auto echo guard. True while bot is speaking. Independent of bIsMuted."))
    void SetSuppressed(bool bSuppressed);

    /**
     * Enable or disable auto-barge-in.
     * When true: if a partial STT result arrives while bIsSuppressed=true, the SDK automatically
     * calls Interrupt() on the owning AEstuaryCharacter (per MIC-06).
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Voice",
              meta = (DisplayName = "Set Auto Barge-In",
                      ToolTip = "Enable auto-barge-in: partial STT while bot speaks triggers Interrupt()."))
    void SetAutoBargeIn(bool bEnable);

    // -----------------------------------------------------------------------
    // BlueprintReadOnly state properties (MIC-05)
    // -----------------------------------------------------------------------

    /** True when user has explicitly muted the microphone. Independent of bIsSuppressed. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Voice",
              meta = (DisplayName = "Is Muted", ToolTip = "True when user-muted. Never auto-toggled."))
    bool bIsMuted = false;

    /** True when auto-suppressed (echo guard while bot is speaking). Independent of bIsMuted. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Voice",
              meta = (DisplayName = "Is Suppressed",
                      ToolTip = "True when auto-suppressed (bot speaking). Never reflects user mute intent."))
    bool bIsSuppressed = false;

    /** True while the RAI capture session is running (between StartListening and StopListening). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Voice",
              meta = (DisplayName = "Is Capturing", ToolTip = "True between StartListening and StopListening."))
    bool bIsCapturing = false;

    /** When true, a partial STT result while bot is speaking triggers automatic Interrupt(). (MIC-06) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Estuary|Voice",
              meta = (DisplayName = "Auto Barge-In", ToolTip = "Enable auto barge-in. Default true."))
    bool bAutoBargeIn = true;

private:

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /** Stop the platform capture device (RAI or built-in FAudioCapture) and clear
     *  its callback. Shared by StopListening (server-notify path) and
     *  StopCaptureLocal (local-release path — no server emits). */
    void StopCaptureDevice();

    /** Bind OnSttResponse on the sibling UEstuaryCharacterComponent for auto-barge-in. */
    void BindSttResponseForBargeIn();

    /** UFUNCTION — receives OnSttResponse from sibling component. Must be UFUNCTION for AddDynamic.
     *  (Pitfall 2: dynamic delegate receivers must be UFUNCTION.) */
    UFUNCTION()
    void HandleSttResponseForBargeIn(const FEstuarySttResponse& Response);

    /**
     * Convert a float32 N-channel raw PCM buffer at SourceSampleRate to int16 mono at 16000 Hz.
     * Called from the capture thread callback — MUST NOT touch any UObject.
     * Uses plain arithmetic only.
     *
     * @param RawFloatPCM   Input: float32 samples, interleaved channels.
     * @param NumSamples    Total number of float samples (frames * channels).
     * @param NumChannels   Channel count in RawFloatPCM (1 or 2 typical).
     * @param SourceRate    Sample rate of RawFloatPCM (e.g., 44100, 48000).
     * @param Out16kMono    Output: int16 samples at 16000 Hz, mono.
     */
    static void ResampleToMono16k(const float* RawFloatPCM, int32 NumSamples,
                                  int32 NumChannels, float SourceRate,
                                  TArray<int16>& Out16kMono);

    // -----------------------------------------------------------------------
    // SPSC queue — ONLY valid data path from capture thread to game thread.
    // Capture thread: Enqueue only.
    // Game thread: Dequeue only (in TickComponent).
    // NEVER access from both threads simultaneously (TQueue<SPSC> guarantees one-producer/one-consumer).
    // -----------------------------------------------------------------------

    /** SPSC queue of raw PCM16 byte chunks ready to base64-encode and emit as stream_audio. */
    TQueue<TArray<uint8>, EQueueMode::Spsc> AudioQueue;

    // -----------------------------------------------------------------------
    // RAI SoundWave handle — only touched on the game thread (BeginPlay, StartListening,
    // StopListening, EndPlay). The capture callback is set at StartListening time and
    // cleared at StopListening time. UPROPERTY to prevent GC collection while capture is live.
    //
    // Stored as opaque UObject pointer so UHT doesn't need to resolve UCapturableSoundWave's
    // UClass when the RuntimeAudioImporter sibling plugin is absent. UHT does not allow
    // UPROPERTY() inside #if (except WITH_EDITORONLY_DATA), so the field is always present;
    // the .cpp casts to UCapturableSoundWave* at use sites under WITH_ESTUARY_RAI.
    // -----------------------------------------------------------------------
    UPROPERTY()
    TObjectPtr<UObject> CapturableWave = nullptr;

    /** Built-in mic fallback session — UE's Audio::FAudioCapture wrapped in a PIMPL so
     *  AudioCaptureCore.h does not need to be included from this public header. Used when
     *  the RuntimeAudioImporter sibling plugin is absent (Windows-only path; cross-platform
     *  builds must install RAI). The PIMPL is created in StartListening, destroyed in
     *  StopListening/EndPlay. Capture callback runs on a worker thread and only
     *  Enqueues to AudioQueue (Pitfall 1). TPimplPtr handles incomplete-type destruction. */
    TPimplPtr<FEstuaryAudioCaptureSession> BuiltInCapture;

    // -----------------------------------------------------------------------
    // Cached sibling references — populated in BeginPlay, cleared in EndPlay.
    // TWeakObjectPtr: guard against destruction between BeginPlay and capture (Pitfall 3).
    // -----------------------------------------------------------------------
    TWeakObjectPtr<UEstuarySubsystem>          WeakSubsystem;
    TWeakObjectPtr<UEstuaryCharacterComponent> WeakCharComp;
};
