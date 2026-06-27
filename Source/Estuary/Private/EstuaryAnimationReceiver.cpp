// EstuaryAnimationReceiver.cpp — UEstuaryAnimationReceiver implementation (Path B).
// Pitfall 1:  All UObject touches on the game thread — TickComponent + BeginPlay guarantee this.
// Pitfall 2:  HandleBotAnimationEvent / HandleInterruptAckedEvent are UFUNCTION() (see header).
// Pitfall 7:  Core has zero LiveLink/* includes — we route through FEstuaryLiveLinkBridge
//             (plain C++ interface, no UObject reflection on the publisher side).
// Pitfall 10: Interpolation done in FAnimationFrameBuffer::GetInterpolatedWeights (not here).
// Pitfall 18: AudioTimeSec comes from UEstuaryAudioPlayerComponent::GetPlaybackTimeSeconds.
// Pitfall 23: IWYU, no CoreMinimal.h.
// Pitfall 25: LiveLinkPose holds the last-pushed frame indefinitely; on interrupt we ramp
//             LastPublishedWeights toward zero over kInterruptRampSecs, publish each ramped
//             frame, then push one final all-zero settle frame to neutralise the cache.
//
// Path B — Live Link publishing pipeline.

#include "EstuaryAnimationReceiver.h"
#include "EstuaryCharacterComponent.h"
#include "EstuaryAudioPlayerComponent.h"
#include "EstuaryReceiverBlendMath.h"
#include "EstuarySubsystem.h"
#include "EstuaryLiveLinkBridge.h"
#include "EstuaryLog.h"
#include "Math/UnrealMathUtility.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/App.h"
#include "HAL/PlatformTime.h"

UEstuaryAnimationReceiver::UEstuaryAnimationReceiver()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UEstuaryAnimationReceiver::BeginPlay()
{
    Super::BeginPlay();
    check(IsInGameThread());

    // Auto-wire to sibling components on the same actor.
    AActor* Owner = GetOwner();
    if (!Owner) return;

    WeakCharComp  = Owner->FindComponentByClass<UEstuaryCharacterComponent>();
    WeakAudioComp = Owner->FindComponentByClass<UEstuaryAudioPlayerComponent>();

    if (!WeakCharComp.IsValid())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[AnimationReceiver] BeginPlay: no sibling UEstuaryCharacterComponent found on '%s'. "
                 "Bind OnBotAnimation and OnInterruptAcked manually."),
            *GetNameSafe(Owner));
    }
    else
    {
        // Pitfall 2: AddDynamic requires UFUNCTION() on the receiver methods.
        WeakCharComp->OnBotAnimation.AddDynamic(
            this, &UEstuaryAnimationReceiver::HandleBotAnimationEvent);
        WeakCharComp->OnInterruptAcked.AddDynamic(
            this, &UEstuaryAnimationReceiver::HandleInterruptAckedEvent);
    }

    if (!WeakAudioComp.IsValid())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[AnimationReceiver] BeginPlay: no sibling UEstuaryAudioPlayerComponent found on '%s'. "
                 "Animation timing will use T=0 until one is present."),
            *GetNameSafe(Owner));
    }

    bSchemaEnsuredForCurrentMessage = false;
}

void UEstuaryAnimationReceiver::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (WeakCharComp.IsValid())
    {
        WeakCharComp->OnBotAnimation.RemoveDynamic(
            this, &UEstuaryAnimationReceiver::HandleBotAnimationEvent);
        WeakCharComp->OnInterruptAcked.RemoveDynamic(
            this, &UEstuaryAnimationReceiver::HandleInterruptAckedEvent);
    }
    FrameBuffer.Clear();
    LastPublishedWeights.Empty();
    Super::EndPlay(EndPlayReason);
}

void UEstuaryAnimationReceiver::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    check(IsInGameThread());

    // Resolve the bridge once per tick. If the EstuaryLiveLink module hasn't
    // registered yet (LiveLinkInterface still loading, or no GameInstance), no-op.
    UWorld* World = GetWorld();
    UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
    UEstuarySubsystem* Sub = GI ? GI->GetSubsystem<UEstuarySubsystem>() : nullptr;
    if (!Sub) { return; }

    FEstuaryLiveLinkBridge& Bridge = Sub->GetLiveLinkBridge();
    if (!Bridge.IsAvailable()) { return; }

    IEstuaryLiveLinkPublisher* Pub = Bridge.GetPublisher();
    if (!Pub) { return; }

    // Resolve subject FName (override -> CharacterId -> OwnerActor name).
    UEstuaryCharacterComponent* CharComp = WeakCharComp.Get();
    const FName SubjectName = CharComp
        ? CharComp->GetLiveLinkSubjectName()
        : (GetOwner() ? GetOwner()->GetFName() : FName(TEXT("EstuaryDefault")));

    // --- Interrupt ramp-to-zero (ANIM-07 / Pitfall 25) ---
    // Buffer was cleared in HandleInterruptAckedEvent; we publish ramped frames toward
    // zero so LiveLinkPose smoothly returns to neutral, then push one all-zero settle
    // frame so the cache neutralises rather than freezing on a near-zero residue.
    if (bRampingToZero)
    {
        // Decay every cached weight toward zero by DeltaTime / kInterruptRampSecs.
        // (DeltaTime / 0.1s = 1.0 over the full ramp; equivalent to RampSpeed=10/s.)
        const float Decay = DeltaTime / kInterruptRampSecs;
        for (auto& Pair : LastPublishedWeights)
        {
            Pair.Value = FMath::Max(0.0f, Pair.Value - Decay);
        }
        if (!LastPublishedWeights.IsEmpty())
        {
            Pub->PublishFrame(SubjectName, LastPublishedWeights,
                              FApp::GetCurrentTime(), CurrentMessageId);
        }

        RampTimeRemaining -= DeltaTime;
        if (RampTimeRemaining <= 0.0f)
        {
            // Push one final all-zero settle frame so LiveLinkPose's last-frame cache
            // holds neutral curves instead of the small residual from the previous tick.
            for (auto& Pair : LastPublishedWeights) { Pair.Value = 0.0f; }
            if (!LastPublishedWeights.IsEmpty())
            {
                Pub->PublishFrame(SubjectName, LastPublishedWeights,
                                  FApp::GetCurrentTime(), CurrentMessageId);
            }
            LastPublishedWeights.Empty();
            bRampingToZero = false;
        }
        return;
    }

    if (!bAnimationEnabled) return;

    // --- Bot-audio gate (Pitfall 25 mirror — natural end-of-utterance) ---
    // The interrupt path (HandleInterruptAckedEvent) starts bRampingToZero on explicit
    // interrupt, but on a NATURAL utterance end (audio finishes, no interrupt event)
    // the audio component's playback time freezes at the last position. Buffered A2F
    // frames keep covering the frozen RelativeAudioTime, so bA2fCovers stays true and
    // we republish the last A2F values forever — face frozen mid-expression.
    //
    // Use the audio component's bIsPlaying flag (exposed via IsBotAudioActive). It
    // flips true when the 200ms pre-buffer is reached + Play() is called, and back to
    // false when the audio component's own TickComponent detects tail-drain (final
    // chunk enqueued + GetAvailableAudioByteCount==0). Authoritative game-thread
    // signal; no envelope/timing heuristics.
    //
    // When inactive: kick off the 100ms ramp-to-neutral if there's a frame on the
    // LiveLinkPose cache, then short-circuit. Critical: short-circuit BOTH cases
    // (with-cleanup and post-cleanup) — otherwise the active path below will see a
    // small residual GetCurrentJawWeight() (engine envelope follower has a 100ms
    // release tail and keeps firing on silence) and Path B re-publishes a tiny
    // {jawOpen: residue} after the ramp completes. The user observes the face
    // "nearly returning to neutral but not quite closing the lips."
    if (UEstuaryAudioPlayerComponent* AudioComp = WeakAudioComp.Get())
    {
        if (!AudioComp->IsBotAudioActive())
        {
            if (!LastPublishedWeights.IsEmpty() && !bRampingToZero)
            {
                FrameBuffer.Clear();
                bRampingToZero       = true;
                RampTimeRemaining    = kInterruptRampSecs;
                FirstA2fFrameTimeSec = -1.0;
                LastA2fEndTimeSec    = -1.0;
                UE_LOG(LogEstuary, Log,
                    TEXT("[AnimationReceiver] Bot audio inactive — beginning 100ms "
                         "ramp-to-neutral (msg=%s)."),
                    *CurrentMessageId);
            }
            // Always return when bot audio is not active. The active path below MUST
            // NOT run during silence — its amplitude branch would publish residual
            // envelope-tail jaw values and re-open the lips a fraction.
            return;
        }
    }

    // --- Per-utterance schema (lazy, idempotent on stable RemapAsset) ---
    // Forwards the RemapAsset to the publisher so it can build the post-remap
    // PropertyNames schema once per (subject, schema) pair (per Task 2 publisher impl).
    // Schema is required for both A2F-driven and amplitude-only paths (the publisher
    // needs the static 52-name schema to zero-pad missing keys per PD-05).
    if (!bSchemaEnsuredForCurrentMessage)
    {
        Pub->EnsureSubject(SubjectName, RemapAsset);
        bSchemaEnsuredForCurrentMessage = true;
    }

    // --- 1. Audio clock (Pitfall 18) ---
    // Use the audio component's playout position as the authoritative time base.
    // If no audio component is present, AudioTimeSec stays 0 — frames play from T=0.
    float AudioTimeSec = 0.0f;
    if (UEstuaryAudioPlayerComponent* AudioComp = WeakAudioComp.Get())
    {
        AudioTimeSec = AudioComp->GetPlaybackTimeSeconds();
    }
    // Convert cumulative audio clock to per-utterance time base. See header comment
    // on MessageAudioOffset for why this is necessary.
    const float RelativeAudioTime = AudioTimeSec - MessageAudioOffset;

    // --- 2. Amplitude-jaw layer (Quick task 260428-365) ---
    // Pull amplitude every tick. Cheap — single member read on the audio component.
    float AmpJaw = 0.0f;
    if (UEstuaryAudioPlayerComponent* AudioComp = WeakAudioComp.Get())
    {
        if (bEnableAmplitudeJawFallback && AmplitudeJawScale > 0.0f)
        {
            AmpJaw = FMath::Clamp(AudioComp->GetCurrentJawWeight() * AmplitudeJawScale,
                                  0.0f, 1.0f);
        }
    }

    // A2F-interpolated weights (existing path) — Pitfall 10.
    // Generalization: FrameBuffer is now TEstuaryAnimationFrameBuffer<FEstuaryBotAnimation>
    // (typedef'd to FAnimationFrameBuffer for backward compat). Call shape preserved via the
    // free-function GetInterpolatedWeights wrapper that delegates to GetInterpolatedFrame().Weights.
    const FEstuaryBotAnimation Interp = FrameBuffer.GetInterpolatedFrame(RelativeAudioTime);
    TMap<FString, float> Weights = Interp.Weights;
    const bool bA2fCovers = (Weights.Num() > 0);

    // Crossfade state-machine update (PD-03 in 260428-365-PLAN.md).
    const double NowSec = FPlatformTime::Seconds();
    if (bA2fCovers && FirstA2fFrameTimeSec < 0.0)
    {
        FirstA2fFrameTimeSec = NowSec;
        LastA2fEndTimeSec    = -1.0;  // (re)enter hold
    }
    else if (!bA2fCovers && FirstA2fFrameTimeSec >= 0.0 && LastA2fEndTimeSec < 0.0)
    {
        LastA2fEndTimeSec = NowSec;  // first tick after A2F coverage ended → start fade-out
    }

    const float Alpha = ComputeCrossfadeAlpha(NowSec);  // 0..1

    // Build OutWeights per branch (per PD-03 + PD-05 zero-pad behavior of publisher).
    TMap<FString, float> OutWeights;
    if (bA2fCovers)
    {
        // Path A: A2F covers. Lerp other 51 curves from 0 → A2F by alpha.
        OutWeights = MoveTemp(Weights);  // 52 keys, base values from A2F
        if (AmpJaw > 0.0f)
        {
            // jawOpen: blend amp UNDER A2F. At α=0 → pure amp; α=1 → max(amp, A2F.jawOpen).
            float A2fJaw = 0.0f;
            if (const float* JawPtr = OutWeights.Find(TEXT("jawOpen"))) { A2fJaw = *JawPtr; }
            const float JawAtFullA2f = FMath::Max(AmpJaw, A2fJaw);
            OutWeights.FindOrAdd(TEXT("jawOpen")) = FMath::Lerp(AmpJaw, JawAtFullA2f, Alpha);
        }
        if (Alpha < 1.0f)
        {
            // Other 51 curves fade in/out with alpha. jawOpen already handled above.
            for (auto& Pair : OutWeights)
            {
                if (Pair.Key != TEXT("jawOpen")) { Pair.Value *= Alpha; }
            }
        }
    }
    else if (AmpJaw > 0.0f)
    {
        // Path B: amplitude-only. Publisher zero-pads other 51 names (verified PD-05 —
        // EstuaryLiveLinkPublisher.cpp:236-249 iterates the static schema and defaults
        // any missing keys to 0.0f). No explicit zero-pad needed in receiver.
        OutWeights.Add(TEXT("jawOpen"), AmpJaw);
    }
    else
    {
        // Path C: layer disabled OR no audio AND no A2F. Publish nothing this tick.
        return;
    }

    // --- 3. Publish to Live Link via bridge ---
    // NOTE: REMAP is NOT applied here. The publisher's BuildSubjectSchema already
    // captured the remap pairings on EnsureSubject; PublishFrame applies ScaleFactor
    // per-curve and looks up by SourceWireName. Single injection point per
    // RESEARCH §"MetaHuman remap injection point" Option A.
    Pub->PublishFrame(SubjectName, OutWeights, FApp::GetCurrentTime(), CurrentMessageId);

    // Cache for ramp-to-zero on next interrupt (Pitfall 25).
    LastPublishedWeights = OutWeights;

    // --- 4. Drift telemetry (ANIM-06) ---
    // Drift > kDriftThresholdMs is logged at most once per second so the diagnostic
    // is visible without flooding the log at 60 Hz tick rate. The exceedance count
    // still increments per tick for accurate stat reporting via GetDriftExceededCount.
    LastDriftMs = FrameBuffer.GetDriftMs(RelativeAudioTime);
    if (FMath::Abs(LastDriftMs) > kDriftThresholdMs)
    {
        ++DriftExceededCount;
        const double NowSeconds = FPlatformTime::Seconds();
        constexpr double kDriftWarnIntervalSeconds = 1.0;
        if (NowSeconds - LastDriftWarnAtSeconds >= kDriftWarnIntervalSeconds)
        {
            LastDriftWarnAtSeconds = NowSeconds;
            UE_LOG(LogEstuary, Verbose,
                TEXT("[AnimationReceiver] drift=%.1fms (count=%d, throttled to 1Hz). "
                     "AudioTimeSec=%.3f RelativeTime=%.3f msg=%s. Tune "
                     "FirstFrameLatencyCompensationMs in the receiver Details panel "
                     "if drift is consistently positive (lipsync trails audio)."),
                LastDriftMs, DriftExceededCount, AudioTimeSec, RelativeAudioTime,
                *CurrentMessageId);
        }
    }
}

void UEstuaryAnimationReceiver::HandleBotAnimationEvent(const FEstuaryBotAnimation& Animation)
{
    check(IsInGameThread());
    if (!bAnimationEnabled) return;

    // New utterance: capture the audio clock as this message's time-base anchor
    // and discard any leftover frames from the prior message. Without this, frames
    // from utterance N+1 (TimeCodeSec ~0) would be compared against the cumulative
    // audio clock (already several seconds in) and dropped as stale.
    //
    // Latency compensation (FirstFrameLatencyCompensationMs UPROPERTY):
    //   The FIRST bot_animation event arrives at the SDK ~A2F-first-frame-latency
    //   AFTER the matching bot_voice audio started filling the audio player.
    //   Worker logs show that latency is consistently 170-240 ms (warm) for the
    //   A2F NIM; UE's audio pre-buffer (200 ms in IsPrimed gate) and the LiveKit
    //   jitter buffer (50-150 ms typical) add more. Without this compensation,
    //   anchoring at first-frame-arrival means frame.TimeCodeSec=0 represents
    //   audio that already played hundreds of ms ago — visible lipsync trails
    //   the audio by that same amount.
    //
    //   Subtracting the compensation makes RelativeAudioTime LARGER for any
    //   given audio clock value, picking the frame that matches the audio
    //   that's actually being heard right now. The UPROPERTY is exposed in the
    //   Details panel so the value can be tuned per-scene without rebuild.
    //
    //   Per ATSC IS-191 lipsync tolerance and NVIDIA ACE A2F UE plugin practice,
    //   biasing slightly so animation LEADS audio by 20-40 ms is more natural
    //   than animation lagging — increase the compensation past the measured
    //   latency by ~30 ms for the best perceived sync.
    const float FirstFrameLatencyCompSec = FirstFrameLatencyCompensationMs / 1000.0f;
    if (!Animation.MessageId.IsEmpty() && Animation.MessageId != CurrentMessageId)
    {
        float NowAudio = 0.0f;
        if (UEstuaryAudioPlayerComponent* AudioComp = WeakAudioComp.Get())
        {
            NowAudio = AudioComp->GetPlaybackTimeSeconds();
        }
        FrameBuffer.Clear();
        CurrentMessageId   = Animation.MessageId;
        MessageAudioOffset = NowAudio - FirstFrameLatencyCompSec;
        // Force the publisher to re-EnsureSubject on this utterance's first tick so any
        // RemapAsset change (rare; usually set once per character) re-establishes schema.
        bSchemaEnsuredForCurrentMessage = false;
        // Reset drift-warn throttle so the FIRST drift warning of a new utterance fires
        // immediately instead of waiting for the 1 Hz window.
        LastDriftWarnAtSeconds = 0.0;
        // Reset crossfade state for the new utterance (PD-04 in 260428-365-PLAN.md).
        // FirstA2fFrameTimeSec re-arms on the first A2F-covering tick of this utterance;
        // LastA2fEndTimeSec re-arms on the first tick after coverage ends.
        FirstA2fFrameTimeSec = -1.0;
        LastA2fEndTimeSec    = -1.0;
        // Abort any in-progress end-of-utterance ramp so this utterance's first frame
        // plays immediately rather than waiting up to 100ms for the ramp to complete.
        bRampingToZero    = false;
        RampTimeRemaining = 0.0f;
        UE_LOG(LogEstuary, Log,
            TEXT("[AnimationReceiver] New utterance msg=%s — anchor=%.3fs "
                 "(NowAudio=%.3f - latencyComp=%.3f). Tune via "
                 "FirstFrameLatencyCompensationMs UPROPERTY."),
            *CurrentMessageId, MessageAudioOffset, NowAudio, FirstFrameLatencyCompSec);
    }

    // Push to buffer. FAnimationFrameBuffer::Push silently ignores terminator frames.
    FrameBuffer.Push(Animation);
}

void UEstuaryAnimationReceiver::HandleInterruptAckedEvent(const FEstuaryInterruptPayload& Payload)
{
    check(IsInGameThread());

    // Clear pending frames immediately — no frozen-face snap (ANIM-07).
    FrameBuffer.Clear();

    // Begin 100 ms ramp-to-zero. The TickComponent ramp loop reads LastPublishedWeights
    // (still populated from the most-recent published frame) and decrements it toward 0,
    // publishing each ramped frame so LiveLinkPose smoothly neutralises. Do NOT clear
    // LastPublishedWeights here — Tick needs it for the ramp; it gets cleared when the
    // ramp completes.
    bRampingToZero    = true;
    RampTimeRemaining = kInterruptRampSecs;

    // Reset crossfade state on interrupt (PD-04 in 260428-365-PLAN.md). The existing
    // kInterruptRampSecs settle ramp on LastPublishedWeights is unchanged — it's
    // the orthogonal "Live Link cache neutralisation" ramp from Pitfall 25.
    FirstA2fFrameTimeSec = -1.0;
    LastA2fEndTimeSec    = -1.0;

    UE_LOG(LogEstuary, Log,
        TEXT("[AnimationReceiver] Interrupt received (message_id=%s). "
             "Buffer cleared. Beginning 100ms ramp-to-zero on Live Link."),
        *Payload.MessageId);
}

void UEstuaryAnimationReceiver::SetEnableAnimation(bool bEnable)
{
    bAnimationEnabled = bEnable;
    if (!bEnable)
    {
        FrameBuffer.Clear();
        LastPublishedWeights.Empty();
        bSchemaEnsuredForCurrentMessage = false;
    }
}

// ============================================================================
// Crossfade alpha helper (Quick task 260428-365)
// ============================================================================

float UEstuaryAnimationReceiver::ComputeCrossfadeAlpha(double NowSec) const
{
    // Pure math lives in EstuaryReceiverBlendMath.h so the same code path is
    // unit-testable without instantiating UObjects. See PD-03 in
    // 260428-365-PLAN.md for the state machine.
    return EstuaryBlend::ComputeCrossfadeAlpha(
        NowSec, FirstA2fFrameTimeSec, LastA2fEndTimeSec, A2FCrossfadeMs);
}
