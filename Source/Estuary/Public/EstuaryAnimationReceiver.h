// EstuaryAnimationReceiver.h — Audio-clock-driven ARKit-52 animation receiver.
// Pitfall 23: #pragma once, IWYU, no CoreMinimal.h.
// Pitfall 10: Interpolation done via FAnimationFrameBuffer::GetInterpolatedWeights.
// Pitfall 18: AudioClockSource->GetPlaybackTimeSeconds() is the time base (not wall clock).
// Pitfall 2:  HandleBotAnimationEvent / HandleInterruptAckedEvent are UFUNCTION for AddDynamic.

#pragma once

#include "CoreTypes.h"
#include "UObject/ObjectMacros.h"
#include "Components/ActorComponent.h"
#include "UObject/WeakObjectPtr.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "EstuaryAnimationFrameBuffer.h"
#include "EstuaryTypes.h"

#include "EstuaryAnimationReceiver.generated.h"

class UEstuaryCharacterComponent;
class UEstuaryAudioPlayerComponent;
class UDataTable;

/**
 * UEstuaryAnimationReceiver
 *
 * Attach to the same AEstuaryCharacter actor as UEstuaryCharacterComponent and
 * UEstuaryAudioPlayerComponent. Auto-wires in BeginPlay via FindComponentByClass.
 *
 * Each TickComponent (Path B — Live Link publishing):
 *   1. Resolves the FEstuaryLiveLinkBridge via UEstuarySubsystem; no-ops if unavailable.
 *   2. Reads audio clock: AudioTimeSec = AudioClockSource->GetPlaybackTimeSeconds()
 *   3. Queries frame buffer: interpolated weights = FrameBuffer.GetInterpolatedWeights(RelativeAudioTime)
 *   4. Calls FEstuaryLiveLinkBridge::GetPublisher()->PublishFrame(SubjectName, Weights,
 *      WorldTime, MessageId). The publisher applies the ARKit→MetaHuman remap internally
 *      and pushes 52 PropertyValues via PushSubjectFrameData_AnyThread.
 *   5. Measures drift = FrameBuffer.GetDriftMs(RelativeAudioTime)
 *      - If |drift| > 100ms: increments DriftExceededCount, emits structured log (ANIM-06)
 *   6. On interrupt: ramp LastPublishedWeights toward zero over kInterruptRampSecs (100 ms),
 *      publish ramped frames each tick, push one final all-zero settle frame so the
 *      LiveLinkPose stale-frame cache neutralises (Pitfall 25).
 */
UCLASS(ClassGroup=(Estuary),
       meta=(BlueprintSpawnableComponent,
             DisplayName="Estuary Animation Receiver",
             ToolTip="Attach to AEstuaryCharacter to receive ARKit-52 lipsync frames and drive UEstuaryFaceAnimInstance at audio-clock-matched timing."))
class ESTUARY_API UEstuaryAnimationReceiver : public UActorComponent
{
    GENERATED_BODY()

public:
    UEstuaryAnimationReceiver();

    // -----------------------------------------------------------------------
    // User-configurable properties
    // -----------------------------------------------------------------------

    /**
     * Optional ARKit remap DataTable (DT_ArkitRemap from Plan 06-04).
     * Forwarded to FEstuaryLiveLinkPublisher::EnsureSubject on the first frame of each
     * utterance. The publisher captures the table's TargetCurveName + ScaleFactor pairs
     * once per (subject, schema) pair and applies them when packing PropertyValues.
     * Leave null to use identity mapping (52 ARKit literals passed through as PropertyNames).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Estuary|Animation",
              meta=(DisplayName="Remap Asset",
                    ToolTip="Optional DataTable for ARKit curve name remapping. Use DT_ArkitRemap with FEstuaryArkitRemapRow for MetaHuman variants."))
    UDataTable* RemapAsset = nullptr;

    /**
     * First-frame latency compensation in milliseconds. Subtracted from the
     * captured audio clock when anchoring MessageAudioOffset for each new
     * utterance. Tunes how far ahead/behind animation lands relative to the
     * audio that's actually being heard.
     *
     * Why it exists: the FIRST bot_animation arrives at the SDK ~A2F-first-frame-
     * latency AFTER the matching bot_voice audio started filling the audio player
     * (worker logs show 170-240 ms warm; cold start 3+ s). The audio pre-buffer
     * (200 ms in USoundWaveProcedural::IsPrimed) and LiveKit jitter buffer
     * (50-150 ms typical) add more. Without compensation, frame.TimeCodeSec=0
     * lines up with audio that played hundreds of ms ago — visible as lipsync
     * trailing the audio.
     *
     * Tuning: increase if lipsync trails audio; decrease if lipsync leads audio.
     * Industry practice (NVIDIA ACE A2F UE plugin, ATSC IS-191 lipsync tolerance)
     * is to bias slightly so animation LEADS audio by 20-40 ms — "leading"
     * lipsync is perceived as more natural than "lagging" lipsync.
     *
     * Default 150 ms (calibrated for amplitude jaw layer ON; if amplitude is
     * disabled via AmplitudeJawScale=0, restore 500 ms) is sized for warm A2F
     * + LiveKit + UE audio pre-buffer in production. Lower values (200-300 ms)
     * work for low-latency local-only setups. The first utterance after a long
     * worker idle window (cold-start A2F TRT engine load) will still trail by
     * 1-3 s no matter what — that's NIM-side, not fixable here.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Estuary|Animation",
              meta=(DisplayName="First Frame Latency Compensation (ms)",
                    ClampMin="0.0",
                    ClampMax="3000.0",
                    UIMin="0.0",
                    UIMax="1500.0",
                    ToolTip="Subtracted from the captured audio clock when anchoring each new utterance. Increase if lipsync lags audio; decrease if lipsync leads. Tune empirically while watching PIE — small changes (50 ms) are perceptible."))
    float FirstFrameLatencyCompensationMs = 150.0f;

    // -----------------------------------------------------------------------
    // Amplitude-driven jawOpen fallback (Quick task 260428-365)
    // -----------------------------------------------------------------------
    // The amplitude layer drives jawOpen from audio amplitude when A2F frames
    // are absent (utterance start-lag, end-truncation, mid-utterance gaps,
    // cold-start before A2F NIM warms). When A2F frames are present, the layer
    // crossfades over A2FCrossfadeMs/2 to full A2F coverage and back when A2F
    // ends. Mirrors the web SDK's the Estuary web SDK reference
    // perceptual feel (pow(env, 0.7) * 0.6) and 200ms inner crossfade.
    // The engine envelope follower (UAudioComponent::OnAudioSingleEnvelopeValueNative)
    // uses peak detection — 10ms attack / 100ms release approximates the web SDK's
    // RMS feel for speech amplitudes (RESEARCH.md §Pitfall 1).

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Estuary|Animation|Amplitude",
              meta=(DisplayName="Enable Amplitude Jaw Fallback",
                    ToolTip="Drives jawOpen from audio amplitude when A2F frames are absent (utterance start, end, gaps, cold-start). Set false to fully disable layer."))
    bool bEnableAmplitudeJawFallback = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Estuary|Animation|Amplitude",
              meta=(DisplayName="Amplitude Jaw Scale",
                    ClampMin="0.0",
                    ClampMax="2.0",
                    UIMin="0.0",
                    UIMax="1.5",
                    ToolTip="Scales the amplitude-driven jaw weight (0.0 disables the layer entirely; 1.0 matches web SDK feel)."))
    float AmplitudeJawScale = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Estuary|Animation|Amplitude",
              meta=(DisplayName="Amplitude Attack (ms)",
                    ClampMin="1.0",
                    ClampMax="200.0",
                    ToolTip="Engine envelope follower attack time. Lower = jaw responds faster to loud transients. Default 10ms tracks speech crisply (engine uses peak detection)."))
    float AmplitudeAttackMs = 10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Estuary|Animation|Amplitude",
              meta=(DisplayName="Amplitude Release (ms)",
                    ClampMin="10.0",
                    ClampMax="500.0",
                    ToolTip="Engine envelope follower release time. Higher = jaw smooths over silence between syllables. Default 100ms matches web SDK perceptual feel."))
    float AmplitudeReleaseMs = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Estuary|Animation|Amplitude",
              meta=(DisplayName="A2F Crossfade (ms)",
                    ClampMin="0.0",
                    ClampMax="1000.0",
                    ToolTip="Total ramp-in + ramp-out blend window between amplitude-only and full A2F. Half goes to fade-in (start of utterance), half to fade-out (end of utterance). Default 200ms; web SDK ships 150ms."))
    float A2FCrossfadeMs = 200.0f;

    // -----------------------------------------------------------------------
    // Blueprint API
    // -----------------------------------------------------------------------

    /**
     * Toggle animation reception. When disabled, incoming frames are discarded
     * (not buffered) to save processing.
     */
    UFUNCTION(BlueprintCallable, Category="Estuary|Animation",
              meta=(DisplayName="Set Enable Animation",
                    ToolTip="Enable or disable animation frame reception and application."))
    void SetEnableAnimation(bool bEnable);

    // -----------------------------------------------------------------------
    // UActorComponent overrides
    // -----------------------------------------------------------------------

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

    // -----------------------------------------------------------------------
    // Diagnostic read-outs (BlueprintPure for stats UI)
    // -----------------------------------------------------------------------

    /** Returns the number of times drift exceeded the 100 ms bound since BeginPlay.
     *  Inspect in Blueprint or stat commands to detect systemic timing issues. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="Estuary|Animation",
              meta=(DisplayName="Get Drift Exceeded Count",
                    ToolTip="Number of ticks where audio-animation drift exceeded 100ms."))
    int32 GetDriftExceededCount() const { return DriftExceededCount; }

    /** Returns the most-recent measured drift in milliseconds (+ahead / -behind). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="Estuary|Animation",
              meta=(DisplayName="Get Last Drift Ms",
                    ToolTip="Most recent audio-animation drift in milliseconds."))
    float GetLastDriftMs() const { return LastDriftMs; }

private:
    // -----------------------------------------------------------------------
    // Internal state
    // -----------------------------------------------------------------------

    FAnimationFrameBuffer FrameBuffer;

    /** Cached sibling component pointers (set in BeginPlay, TWeakObjectPtr — Pitfall 3). */
    TWeakObjectPtr<UEstuaryCharacterComponent>   WeakCharComp;
    TWeakObjectPtr<UEstuaryAudioPlayerComponent> WeakAudioComp;

    bool  bAnimationEnabled  = true;
    bool  bRampingToZero     = false;
    float RampTimeRemaining  = 0.0f;

    /** Drift telemetry (ANIM-06). */
    int32 DriftExceededCount = 0;
    float LastDriftMs        = 0.0f;

    /** Wall-clock seconds (FPlatformTime::Seconds) of the most-recent drift warning;
     *  used to throttle drift warnings to ~1 Hz instead of per-tick. */
    double LastDriftWarnAtSeconds = 0.0;

    /** Crossfade state — set on first A2F frame of an utterance / first tick after
     *  A2F coverage ends. -1.0 sentinel = unset. Reset per-utterance and on interrupt.
     *  See PD-03 in 260428-365-PLAN.md and EstuaryReceiverBlendMath::ComputeCrossfadeAlpha
     *  for the full state machine. */
    double FirstA2fFrameTimeSec = -1.0;
    double LastA2fEndTimeSec    = -1.0;

    /** Per-utterance audio-clock offset.
     *  GetPlaybackTimeSeconds() is cumulative across the lifetime of the audio
     *  player, but A2F's TimeCodeSec restarts at ~0 each utterance. Without an
     *  offset, the second utterance compares fresh frames at TimeCodeSec=0 against
     *  an audio clock at 10s+ → drift = full prior-audio length, frames look
     *  perpetually stale, the publisher reads zero curves. We capture the audio
     *  clock value when each new MessageId's first frame arrives, then subtract
     *  it to recover a per-utterance time base. */
    FString CurrentMessageId;
    float   MessageAudioOffset = 0.0f;

    /** Last interpolated frame published — used during interrupt ramp to scale toward zero.
     *  Cleared after the ramp completes (one zero settle frame is published at the end).
     *  Pitfall 25: LiveLinkPose holds the last-pushed frame indefinitely; we cannot just
     *  stop publishing on interrupt or the face freezes mid-expression. */
    TMap<FString, float> LastPublishedWeights;

    /** True only on the first frame of a new utterance; the receiver calls EnsureSubject
     *  on the publisher to (re)build per-subject schema with current RemapAsset. */
    bool bSchemaEnsuredForCurrentMessage = false;

    static constexpr float kDriftThresholdMs  = 100.0f;
    static constexpr float kInterruptRampSecs = 0.1f;   // 100 ms ramp on interrupt

    // -----------------------------------------------------------------------
    // UFUNCTION delegate receivers (Pitfall 2: must be UFUNCTION for AddDynamic)
    // -----------------------------------------------------------------------

    /** Bound to UEstuaryCharacterComponent::OnBotAnimation in BeginPlay. */
    UFUNCTION()
    void HandleBotAnimationEvent(const FEstuaryBotAnimation& Animation);

    /** Bound to UEstuaryCharacterComponent::OnInterruptAcked in BeginPlay.
     *  Clears buffer and begins 100 ms ramp-to-zero (ANIM-07). */
    UFUNCTION()
    void HandleInterruptAckedEvent(const FEstuaryInterruptPayload& Payload);

    /** Crossfade alpha helper — wraps EstuaryBlend::ComputeCrossfadeAlpha with the
     *  receiver's tunables and crossfade state. Returns 0..1: 0 = amplitude-only,
     *  1 = full A2F coverage. See PD-03 in 260428-365-PLAN.md. */
    float ComputeCrossfadeAlpha(double NowSec) const;
};
