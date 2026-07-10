// EstuaryBodyAnimationReceiver.cpp — UEstuaryBodyAnimationReceiver production impl.
//
// Pitfall 1:  All UObject touches on the game thread — TickComponent + BeginPlay guarantee this.
// Pitfall 2:  HandleBotPoseEvent / HandleInterruptAckedEvent are UFUNCTION() (see header).
// Pitfall 3:  TWeakObjectPtr cached in BeginPlay; stale-pointer defense via .Get() null-checks.
// Pitfall 6:  LastPublishedTransforms.Reserve(EstuaryMHR::kNumBones) at construction;
//             FQuat::Slerp + FVector::Lerp don't allocate.
// Pitfall 7:  ZERO LiveLink/* includes — receiver consumes IEstuaryBodyLiveLinkPublisher plain
//             C++ interface via FEstuaryLiveLinkBridge service-locator.
// Pitfall 18: AudioTimeSec comes from UEstuaryAudioPlayerComponent::GetPlaybackTimeSeconds.
//             SAME clock as face — cross-modality drift impossible by construction.
// Pitfall 23: IWYU, no CoreMinimal.h.
// Pitfall 25: LiveLinkPose holds last-pushed frame indefinitely; the HOLD path keeps the
//             cache actively pinned by re-publishing the LAST pose while the buffer is
//             empty, and the resume blend slerps into the returning stream over
//             kResumeBlendSecs. (2026-06-10: replaced the ramp-to-bind-pose — identity
//             quats == T-pose under the VRM identity-bind convention.)
//
// REPLACES the earlier first-pass scaffold. BeginPlay/EndPlay binding pattern
// preserved verbatim from the scaffold (auto-wire to sibling UEstuaryCharacterComponent +
// UEstuaryAudioPlayerComponent; OnBotPose AddDynamic; OnBotPose RemoveDynamic on EndPlay).
// HandleBotPoseEvent body REPLACED — no longer captures the diagnostic pose snapshot;
// instead resets per-utterance state on MessageId change + pushes to Buffer.

#include "EstuaryBodyAnimationReceiver.h"
#include "EstuaryLog.h"
#include "EstuaryBodyFrameTransform.h"
#include "EstuaryMHRRig.h"
#include "EstuaryPlayheadMath.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"

// 2026-07-02 MHR migration: the 52-entry SMPL-X wire→skeleton name-remap TMap
// (RemapGestureSyncToSkeletonBoneName) that lived here is DELETED. The generated
// MHR FBX carries the wire bone names verbatim (root, l_upleg, ..., c_head), so
// the receiver publishes wire FNames directly; topology constants come from the
// generated EstuaryMHRRig.h (authority: SARAH models/rig.py via
// scripts/generate_mhr_wire_assets.py).

#include "EstuaryAnimationFrameBuffer.h"
#include "EstuaryCharacterComponent.h"
#include "EstuaryAudioPlayerComponent.h"
#include "EstuaryLiveLinkBridge.h"
#include "EstuarySubsystem.h"
#include "EstuaryDeveloperSettings.h"  // project-wide DefaultFingerPolicy default
#include "EstuaryLog.h"
#include "EstuaryTypes.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Misc/App.h"
#include "EngineUtils.h"  // TActorIterator for the GestureSyncSourceMesh asset force-set scan
#include "Math/UnrealMathUtility.h"
#include "Math/Quat.h"
#include "Math/Transform.h"
#include "Math/Vector.h"

// Force-emit FEstuaryLiveLinkBridge::RegisterBodyPublisher / UnregisterBodyPublisher into
// the Estuary DLL's export table. Without this, MSVC inlines them everywhere they are
// called but never emits an out-of-line copy; the EstuaryLiveLink module's TU then
// generates a dllimport reference (because FEstuaryLiveLinkBridge has ESTUARY_API) and
// the cold link fails with LNK2019 (and Live Coding's incremental patch can't add new
// cross-module exports). Taking the address of each method via a `[[maybe_unused]]
// constexpr` member-pointer constant forces the compiler to emit the function body in
// this TU and export it.
namespace
{
    using BridgeRegisterBodyFn   = void (FEstuaryLiveLinkBridge::*)(IEstuaryBodyLiveLinkPublisher*);
    using BridgeUnregisterBodyFn = void (FEstuaryLiveLinkBridge::*)();
    [[maybe_unused]] constexpr BridgeRegisterBodyFn   _ForceEmitRegisterBody   = &FEstuaryLiveLinkBridge::RegisterBodyPublisher;
    [[maybe_unused]] constexpr BridgeUnregisterBodyFn _ForceEmitUnregisterBody = &FEstuaryLiveLinkBridge::UnregisterBodyPublisher;
}
#include "Math/Vector4.h"
#include "HAL/PlatformTime.h"

UEstuaryBodyAnimationReceiver::UEstuaryBodyAnimationReceiver()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
    // Pitfall 6: pre-allocate the per-tick scratchpad so the publish path never reallocates.
    LastPublishedTransforms.Reserve(EstuaryMHR::kNumBones);
}

void UEstuaryBodyAnimationReceiver::BeginPlay()
{
    Super::BeginPlay();
    check(IsInGameThread());

    // ---------------------------------------------------------------------
    // Honor project-wide DefaultFingerPolicy when the
    // per-receiver UPROPERTY is still at the C++ default (Driven). BP-set
    // values always win because BP serializes the property — sentinel
    // approach mirrors UE convention.
    // ---------------------------------------------------------------------
    if (const UEstuaryDeveloperSettings* Settings = GetDefault<UEstuaryDeveloperSettings>())
    {
        if (FingerPolicy == EEstuaryFingerPolicy::Driven /* C++ default; BP-set values survive */)
        {
            FingerPolicy = Settings->DefaultFingerPolicy;
            UE_LOG(LogEstuary, Verbose,
                TEXT("[BodyAnimReceiver] FingerPolicy set from project DefaultFingerPolicy=%d "
                     "(BP-set values would have overridden — UE convention)."),
                static_cast<int32>(FingerPolicy));
        }
    }

    // ---------------------------------------------------------------------
    // Auto-wire to sibling components (preserve scaffold pattern verbatim).
    // ---------------------------------------------------------------------
    AActor* Owner = GetOwner();
    if (!Owner)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[BodyAnimReceiver] BeginPlay: no owning actor — bind skipped."));
        return;
    }

    WeakCharComp  = Owner->FindComponentByClass<UEstuaryCharacterComponent>();
    WeakAudioComp = Owner->FindComponentByClass<UEstuaryAudioPlayerComponent>();

    // 2026-05-04 IKR refactor: ensure GestureSyncSourceMesh component (if present anywhere
    // in the world) has its SkeletalMeshAsset assigned. MetaHumanComponentUE clears
    // non-MH skel mesh assets at runtime even though the BP template has them set.
    // Runs unconditionally.
    //
    // 2026-05-05 cleanup (punchlist item 3): force-set is unconditional — do NOT gate
    // on `GetSkeletalMeshAsset() == nullptr`. MetaHumanComponentUE can clear the asset
    // AFTER our BeginPlay runs, so a one-shot null check at BeginPlay misses that
    // window and leaves the mesh permanently cleared with no recovery (verified
    // 2026-05-05 18:14 incident — receiver claimed force-set succeeded but live mesh
    // was clear by the time we probed). Always overwrite.
    {
        if (UWorld* World = GetWorld())
        {
            for (TActorIterator<AActor> It(World); It; ++It)
            {
                AActor* Actor = *It;
                if (!Actor) continue;
                TArray<USkeletalMeshComponent*> Skels;
                Actor->GetComponents<USkeletalMeshComponent>(Skels);
                for (USkeletalMeshComponent* C : Skels)
                {
                    if (!C) continue;
                    if (C->GetName().Equals(TEXT("GestureSyncSourceMesh"), ESearchCase::IgnoreCase))
                    {
                        USkeletalMesh* SK = LoadObject<USkeletalMesh>(nullptr,
                            TEXT("/Game/Estuary/IKRetargeters/SK_GestureSyncSource.SK_GestureSyncSource"));
                        if (SK)
                        {
                            C->SetSkeletalMeshAsset(SK);
                            UClass* AnimCls = LoadObject<UClass>(nullptr,
                                TEXT("/Game/Estuary/IKRetargeters/ABP_GestureSyncSourceLiveLink.ABP_GestureSyncSourceLiveLink_C"));
                            if (AnimCls)
                            {
                                C->SetAnimInstanceClass(AnimCls);
                            }
                            UE_LOG(LogEstuary, Log,
                                TEXT("[BodyAnimReceiver] BeginPlay: force-set GestureSyncSourceMesh.SkeletalMeshAsset on actor '%s' (unconditional — MetaHumanComponentUE may clear post-BeginPlay)."),
                                *GetNameSafe(Actor));
                        }
                        break;
                    }
                }
            }
        }
    }

    if (UEstuaryCharacterComponent* CharComp = WeakCharComp.Get())
    {
        // Pitfall 8: AddDynamic requires UFUNCTION() on the receiver method (see header).
        CharComp->OnBotPose.AddDynamic(
            this, &UEstuaryBodyAnimationReceiver::HandleBotPoseEvent);
        CharComp->OnInterruptAcked.AddDynamic(
            this, &UEstuaryBodyAnimationReceiver::HandleInterruptAckedEvent);

        UE_LOG(LogEstuary, Log,
            TEXT("[BodyAnimReceiver] BeginPlay: bound OnBotPose + OnInterruptAcked on '%s'."),
            *GetNameSafe(Owner));
    }
    else
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[BodyAnimReceiver] BeginPlay: no sibling UEstuaryCharacterComponent "
                 "found on '%s'. Bind OnBotPose + OnInterruptAcked manually if needed."),
            *GetNameSafe(Owner));
    }

    if (!WeakAudioComp.IsValid())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[BodyAnimReceiver] BeginPlay: no sibling UEstuaryAudioPlayerComponent "
                 "found on '%s'. Body-animation timing will use T=0 until one is present "
                 "(face and body share the SAME audio clock — cross-modality)."),
            *GetNameSafe(Owner));
    }

    bSchemaEnsuredForCurrentMessage = false;

    // 2026-06-10: the bundled bind-pose JSON load was removed along with the
    // interrupt ramp-to-bind-pose. The file shipped all-identity quats — the
    // T-pose under the VRM identity-bind convention — AND the cache was keyed
    // by GestureSync wire names while the ramp looked it up by post-remap
    // skeleton names, so the lookup missed to FQuat::Identity anyway. (The
    // JSON file itself was deleted in Task 17 — it had zero remaining consumers.)
    // The server-side bind registry is unaffected.
}

void UEstuaryBodyAnimationReceiver::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // Preserve scaffold's unbinding pattern + add OnInterruptAcked unbind.
    if (UEstuaryCharacterComponent* CharComp = WeakCharComp.Get())
    {
        CharComp->OnBotPose.RemoveDynamic(
            this, &UEstuaryBodyAnimationReceiver::HandleBotPoseEvent);
        CharComp->OnInterruptAcked.RemoveDynamic(
            this, &UEstuaryBodyAnimationReceiver::HandleInterruptAckedEvent);
    }
    Buffer.Clear();
    LastPublishedTransforms.Reset();
    bResumeBlendPending = false;
    bResumeBlending = false;
    ResumeBlendTimeAccumulated = 0.0f;
    ResumeBlendSourceTransforms.Reset();
    InterruptedMessageId.Empty();
    Super::EndPlay(EndPlayReason);
}

// ---------------------------------------------------------------------------
// Subject naming helper — "<characterId>_Body_GestureSync".
// Single-place enforcement of the body subject convention. Face stays bare-id
// (so v1.0 face ABPs continue to work); body always suffixes "_Body_GestureSync"
// so face and body subjects can coexist on the same actor.
// ---------------------------------------------------------------------------
FName UEstuaryBodyAnimationReceiver::ResolveSubjectName() const
{
    // 2026-05-04 IKR refactor: subject now publishes GestureSync-named bones (not MH).
    // The hidden SK_GestureSyncSource SkeletalMeshComponent on the character consumes
    // this subject via LiveLinkController; ABP_Body uses Retarget Pose From Mesh
    // sourced from that hidden component to drive the MH skel via IKR_GestureSyncToMetaHuman.
    if (UEstuaryCharacterComponent* CharComp = WeakCharComp.Get())
    {
        const FName BareName = CharComp->GetLiveLinkSubjectName();
        return FName(*FString::Printf(TEXT("%s_Body_GestureSync"), *BareName.ToString()));
    }
    if (AActor* Owner = GetOwner())
    {
        return FName(*FString::Printf(TEXT("%s_Body_GestureSync"), *Owner->GetFName().ToString()));
    }
    return FName(TEXT("default_Body_GestureSync"));
}

// ---------------------------------------------------------------------------
// TickComponent — audio-clock-driven interpolate + convert + remap + publish.
// Mirrors UEstuaryAnimationReceiver::TickComponent (face) structure verbatim,
// adapted to body's transform-array data shape (vs face's TMap<FString,float>).
// ---------------------------------------------------------------------------
void UEstuaryBodyAnimationReceiver::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    check(IsInGameThread());

    // Resolve the bridge once per tick. If the EstuaryLiveLink module hasn't
    // registered the body publisher yet (LiveLinkInterface still loading, or
    // no GameInstance), no-op gracefully.
    UWorld* World = GetWorld();
    UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
    UEstuarySubsystem* Sub = GI ? GI->GetSubsystem<UEstuarySubsystem>() : nullptr;
    if (!Sub) { return; }

    // -----------------------------------------------------------------
    // Rig gate (SDK_CONTRACT §session_info.animation_capabilities.body.rig;
    // spec §8 — loud failure, no silent mixed-version frame drops). This
    // receiver speaks ONLY the MHR63 wire; if the server declares any other
    // rig, disable the component tick permanently with ONE Error log. The
    // check latches once session_info has landed (SessionId non-empty);
    // servers that omit the rig field (bPresent false or empty Rig) pass —
    // the ==63 ingest count gate below still catches a wrong wire.
    // -----------------------------------------------------------------
    if (!bRigGateLatched)
    {
        const FEstuarySessionInfo Info = Sub->GetSessionInfo();
        if (!Info.SessionId.IsEmpty())
        {
            if (Info.AnimationCapabilities.bPresent
                && !Info.AnimationCapabilities.Body.Rig.IsEmpty()
                && Info.AnimationCapabilities.Body.Rig != TEXT("mhr63"))
            {
                UE_LOG(LogEstuary, Error,
                    TEXT("[BodyAnimReceiver] session_info body.rig='%s' != 'mhr63' — this "
                         "client only speaks the MHR63 wire. Body animation DISABLED for "
                         "this session (component tick off). Update the server or the "
                         "plugin so both sides are on the same rig."),
                    *Info.AnimationCapabilities.Body.Rig);
                SetComponentTickEnabled(false);
                return;
            }
            bRigGateLatched = true;
        }
    }

    FEstuaryLiveLinkBridge& Bridge = Sub->GetLiveLinkBridge();
    if (!Bridge.IsBodyAvailable()) { return; }

    IEstuaryBodyLiveLinkPublisher* Pub = Bridge.GetBodyPublisher();
    if (!Pub) { return; }

    // Resolve subject FName ("<characterId>_Body_GestureSync").
    const FName SubjectName = ResolveSubjectName();

    // 2026-06-10: the interrupt ramp-to-bind-pose block that lived here was
    // REMOVED. Its target ("bind pose") was all-identity quats == T-pose under
    // the VRM identity-bind convention, so every barge-in visibly snapped
    // the character to T-pose. Interrupts now fall through to the HOLD path
    // below (buffer was cleared by HandleInterruptAckedEvent), which re-publishes
    // the last pose until the idle pump resumes the stream; the resume blend in
    // the active path conceals the seam.

    if (!bAnimationEnabled) { return; }

    // -----------------------------------------------------------------
    // Audio clock (Pitfall 18).
    // SAME WeakAudioComp->GetPlaybackTimeSeconds() that the face receiver
    // reads — cross-modality drift impossible by construction.
    // -----------------------------------------------------------------
    float AudioTimeSec = 0.0f;
    if (UEstuaryAudioPlayerComponent* AudioComp = WeakAudioComp.Get())
    {
        AudioTimeSec = AudioComp->GetPlaybackTimeSeconds();
    }
    // Idle messages (worker silence-fill) use a wall-clock anchor because
    // GetPlaybackTimeSeconds() freezes between bot utterances and would map
    // every idle frame to the same RelativeAudioTime → frozen pose.
    float RelativeAudioTime = bUseWallClockForCurrentMessage
        ? static_cast<float>(FApp::GetCurrentTime() - MessageWallClockOffset)
        : (AudioTimeSec - MessageAudioOffset);

    // -----------------------------------------------------------------
    // Bind-pose hold during pre-buffer / drop-out.
    // When the buffer is empty (200 ms pre-buffer at session start) OR the
    // audio clock is too far ahead of the most recent frame (gap > 100 ms =
    // drop-out), publish a single bind-pose-shaped frame so the LiveLinkPose
    // Body node doesn't freeze on the prior pose (Pitfall 25 — LiveLinkPose
    // stale-frame cache). Bind-pose hold is the no-frames-yet behavior for
    // ALL bones, not just fingers — fingers are the most visible failure
    // case but the same logic applies to body 0..21 during pre-buffer.
    // -----------------------------------------------------------------
    // 2026-05-04 FIX-4: drift-based bind-pose-hold gating was wrong for this
    // pipeline. GestureSync's PCM-batched + GPU + network path naturally produces
    // frames 250-500 ms behind real-time, so the 100 ms drift threshold
    // permanently triggered HOLD once the first frame arrived — buffer grew
    // unbounded (verified: 456 frames buffered, never consumed) and the
    // character was stuck at MetaHuman bind pose forever.
    //
    // Bind-pose-hold's only legitimate purpose is the genuine drop-out case
    // (Pitfall 25 — LiveLinkPose stale-frame cache when frames stop flowing).
    // GetInterpolatedFrame already clamps to the latest frame when AudioTime
    // exceeds it (line 199 of EstuaryAnimationFrameBuffer.cpp), so we can
    // safely consume even when "behind."
    //
    // Additionally: resync the playhead anchor when it drifts from the buffer's
    // newest frame so we drain at GestureSync's natural production rate instead of
    // accumulating forever (idle) or starving (speech) — adaptive slew below.
    const float LatestFrameT = Buffer.HasFrames() ? Buffer.GetLatestFrameTime() : 0.0f;
    if (Buffer.HasFrames())
    {
        // GestureSync's TimeCodeSec advances based on the worker's cumulative
        // chunk counter (8 frames * seq_num / 30 fps), which drifts vs
        // wall-clock in BOTH directions: production runs ~1.86x real-time
        // when GPU is hot (pump < 20ms sleep), or slower during backpressure.
        //
        // Wall-clock-only RelT advancement (2026-05-05 v2 fix).
        //
        // Constraint: GestureSync idle pump produces frames with TimeCodeSec advancing
        // at 30 fps cadence, BUT Triton emits chunks faster than wall-clock during
        // idle (verified: ~64 frames/sec at gateway = 2.13x real-time). LatestFrameT
        // therefore races ahead. Earlier "smooth-with-cap" pinned RelT to a window
        // around LatestFrameT, which made playback follow that 2.13x race → motion
        // appeared at fast-forward speed.
        //
        // Solution: RelT always advances at 1.0x wall-clock. Buffer can sit arbitrarily
        // far ahead — frames are consumed at the natural 30 fps cadence dictated by
        // their TimeCodeSec values, not by GestureSync's variable production rate. Excess
        // frames accumulate at buffer head (capacity-pruned at oldest end).
        //
        // Rescue: only re-anchor MessageWallClockOffset when RelT diverges from the
        // buffer's content by >2 seconds (catastrophic — e.g. PIE pause, message
        // rollover races, or first-frame anchoring missed). Otherwise free-run.
        // 2026-06-09: adaptive-playout slew (replaces pure free-run). GestureSync delivers
        // 8-frame chunks every ~267ms; a free-run playhead with only 150ms lead
        // outran the supply within each chunk gap and micro-stuttered (clamp-on-
        // latest, leadMs sawtoothing -200..+30). We now (a) target a lead larger
        // than one chunk (kTargetLagSec=0.40) and (b) nudge RelT a few % toward that
        // target each tick instead of free-running, so the 267ms chunk steps smooth
        // out and the playhead never passes the newest frame.
        //
        // Why the old "free-run, never cap" comment no longer applies: it feared
        // fast-forward from LatestFrameT racing at ~2.13x — but that was the worker
        // OVER-feeding Triton. The user-PCM coalescing fix (backend-side) restored
        // real-time (1.0x) supply, so tracking (LatestFrameT - lead) holds steady 1x
        // playback. The slew also degrades gracefully if supply ever races (a mild
        // speedup over ~0.3s, not a hard snap). In steady state it is monotonic-
        // forward (per tick RelT advances ~dt and the pull toward target only
        // SLOWS it); right after an anchor that starts ahead of the target (AUDIO
        // utterance start) the pull can briefly exceed dt — sub-frame backward
        // steps while the displayed pose is clamp-held anyway, never a visible
        // rewind.
        //
        // 2026-06-10: the slew now ALSO runs for AUDIO (speech) utterances, trimming
        // MessageAudioOffset instead of MessageWallClockOffset. Speech frames are
        // supplied at only ~1x real-time (GestureSync inference is paced by the live TTS
        // audio), but the audio-clock anchor started the playhead +latencyComp AHEAD
        // of the frame timeline with NO lead control, so leadMs sat at -13..-400 for
        // entire utterances (PIE 2026-06-10 logs) and GetInterpolatedFrame clamped to
        // the newest frame — the pose only changed when a chunk landed, i.e. speech
        // looked like a series of freeze frames while idle (which had this slew) was
        // smooth. The audio clock stays the free-run time-base for speech, so playback
        // still pauses with the audio; the slew only maintains ~kTargetLagSec of
        // buffered frames, mirroring the web client's adaptive-playout corrector. Body
        // gesture therefore trails the voice by ~0.4 s — well inside gesture-sync
        // tolerance, unlike lipsync; the FACE receiver is untouched and stays tightly
        // audio-anchored (its A2F supply bursts well ahead of real-time, so it never
        // starves).
        //
        // Math + constants live in EstuaryPlayheadMath.h (kTargetLagSec=0.40,
        // kSlewAlpha=0.05, kCatastrophicDriftSec=2.0) with unit tests under
        // "Estuary.Playhead.*" (Private/Tests/EstuaryPlayheadSlewTests.cpp).
        EstuaryPlayhead::FSlewResult Corrected;
        if (bUseWallClockForCurrentMessage)
        {
            const double Now          = FApp::GetCurrentTime();
            const float  UncappedRelT = static_cast<float>(Now - MessageWallClockOffset);
            Corrected = EstuaryPlayhead::SlewPlayhead(UncappedRelT, LatestFrameT, DeltaTime);
            MessageWallClockOffset = Now - static_cast<double>(Corrected.RelT);
            RelativeAudioTime      = Corrected.RelT;
        }
        else
        {
            const float UncappedRelT = AudioTimeSec - MessageAudioOffset;
            Corrected = EstuaryPlayhead::SlewPlayhead(UncappedRelT, LatestFrameT, DeltaTime);
            MessageAudioOffset = AudioTimeSec - Corrected.RelT;
            RelativeAudioTime  = Corrected.RelT;
        }
        if (Corrected.bReanchored)
        {
            ++DiagReanchorWindow;
            // A hard re-anchor jumps the playhead by up to kCatastrophicDriftSec
            // (PIE pause, audio-clock reset after an interrupt flush, message
            // rollover) — conceal the pose discontinuity with the resume slerp.
            bResumeBlendPending = true;
        }
    }
    const float GapMs = Buffer.HasFrames() ? Buffer.GetDriftMs(RelativeAudioTime) : -1.0f;
    const bool bGoToHold = !Buffer.HasFrames();
    if (bGoToHold) { ++DiagHoldTicksWindow; }
    // [BodyDiag] consolidated lag telemetry, throttled to ~1 Hz (Display level so
    // it shows in normal logs without Warning spam). Localizes "laggy" playback:
    //   recvFps  — frame supply rate; ~30 if GestureSync keeps up, < 30 = starvation.
    //   leadMs   — buffer head minus playhead; targets +400ms (kTargetLagSec, both
    //              modes). negative = late/starved (playhead outran supply); large
    //              & growing = over-buffered.
    //   driftMs  — GetDriftMs(playhead). reanchor/holdTicks = catastrophic playhead
    //              re-anchors (either clock mode) and HOLD (underrun) ticks in the window.
    {
        const double NowL = FPlatformTime::Seconds();
        const double WindowSec = NowL - DiagLastLogSeconds;
        if (WindowSec > 1.0)
        {
            const float RecvFps = (WindowSec > 0.0) ? (DiagFramesRecvWindow / static_cast<float>(WindowSec)) : 0.0f;
            const float LeadMs  = Buffer.HasFrames() ? (LatestFrameT - RelativeAudioTime) * 1000.0f : 0.0f;
            UE_LOG(LogEstuary, Display,
                TEXT("[BodyDiag] %s mode=%s buf=%d recvFps=%.1f leadMs=%.0f driftMs=%.1f relT=%.2f latestT=%.2f reanchor=%d holdTicks=%d audioClk=%.2f msg=%s"),
                bGoToHold ? TEXT("HOLD") : TEXT("ACTIVE"),
                bUseWallClockForCurrentMessage ? TEXT("IDLE-wallclock") : TEXT("AUDIO"),
                Buffer.Num(), RecvFps, LeadMs, GapMs, RelativeAudioTime, LatestFrameT,
                DiagReanchorWindow, DiagHoldTicksWindow, AudioTimeSec,
                CurrentMessageId.IsEmpty() ? TEXT("<none>") : *CurrentMessageId);
            DiagLastLogSeconds   = NowL;
            DiagFramesRecvWindow = 0;
            DiagReanchorWindow   = 0;
            DiagHoldTicksWindow  = 0;
        }
    }
    if (bGoToHold)
    {
        // HOLD-LAST-POSE (2026-06-10, replaces the identity "bind-pose hold").
        // No frames buffered: post-interrupt blackout (worker cancels the
        // utterance, idle pump's first frame needs ~0.3-1 s), session-start
        // pre-buffer, or a supply drop-out. Re-publish the LAST pose each tick
        // (Pitfall 25 — keeps the LiveLinkPose cache actively pinned) and arm
        // the resume blend so the returning stream slerps in instead of
        // snapping. The old path published identity rotations here — the
        // T-pose under the VRM identity-bind convention — which is why
        // every barge-in T-posed the character for the length of the blackout.
        //
        // Schema not yet known (no utterance has ever arrived) or nothing ever
        // published: nothing to hold; LiveLinkPose Body shows its neutral cache.
        if (CachedBoneNames.Num() == 0 || LastPublishedTransforms.Num() == 0) { return; }

        bResumeBlendPending = true;
        Pub->PublishFrame(SubjectName, LastPublishedTransforms,
                          FApp::GetCurrentTime(), CurrentMessageId);
        return;
    }

    // -----------------------------------------------------------------
    // Interpolate against audio time (per-bone Slerp + per-hips Lerp).
    // -----------------------------------------------------------------
    const FEstuaryBotPose Interp = Buffer.GetInterpolatedFrame(RelativeAudioTime);
    if (Interp.Bones.Num() == 0) { return; }
    // J=63 enforced at the ingest gate (HandleBotPoseEvent); iterate
    // Bones.Num() defensively anyway.
    const int32 NumBones = Interp.Bones.Num();

    // (2026-06-09) Removed the 4 Hz [DIAG] per-bone quat dump — it was the main
    // log-spam offender and is superseded by the throttled [BodyDiag] telemetry
    // above (which already reports buffer depth + supply rate + lead). Re-add at
    // Verbose if a per-bone value trace is ever needed again.

    // -----------------------------------------------------------------
    // Build schema (lazy, idempotent — first frame of each new utterance).
    // Schema = (BoneNames, BoneParents) tuple passed to publisher's
    // EnsureSubject so the Live Link static data is built once per
    // (subject, schema) pair. The downstream IK Retargeter asset is the actual
    // parent map; the BoneParents here is informational for the Live Link panel.
    // -----------------------------------------------------------------
    if (!bSchemaEnsuredForCurrentMessage)
    {
        // MHR63 topology from the generated EstuaryMHRRig.h (authority: SARAH
        // models/rig.py MHR_J63_PARENTS via scripts/generate_mhr_wire_assets.py).
        // Wire bone names ARE the SK_GestureSyncSource skeleton bone names (the
        // FBX is generated in wire order/names), so the schema publishes wire
        // FNames directly — no name-remap layer. A correct parent table matters:
        // a prior placeholder ("everything child of root") corrupted the LiveLink
        // skeleton hierarchy and broke parent-relative pose evaluation in
        // LiveLinkPose. The downstream IK Retargeter asset is the actual
        // cross-skeleton map; this hierarchy feeds the Live Link panel +
        // LiveLinkPose evaluation.
        CachedBoneNames.Reset(NumBones);
        CachedBoneParents.Reset(NumBones);
        for (int32 i = 0; i < NumBones; ++i)
        {
            CachedBoneNames.Add(FName(*Interp.Bones[i].Name));
            const int32 ParentIdx = (i < EstuaryMHR::kNumBones) ? EstuaryMHR::kBoneParents[i] : -1;
            CachedBoneParents.Add(ParentIdx);
        }
        Pub->EnsureSubject(SubjectName, CachedBoneNames, CachedBoneParents);
        bSchemaEnsuredForCurrentMessage = true;
    }

    // -----------------------------------------------------------------
    // MHR→UE conversion into LastPublishedTransforms. No name-remap layer:
    // wire bone names ARE the SK_GestureSyncSource skeleton names (generated
    // MHR FBX in wire order/names).
    // EstuaryBodyFrameTransform::ToUEQuatFromMHR — PROVISIONAL component map
    // until the Phase 5 FK solve against the imported MHR skeleton re-pins it.
    // EstuaryBodyFrameTransform::ToUEHipsFromMHR — meters-passthrough Y-mirror
    // (units decision re-derived in Phase 5, documented in the header).
    // -----------------------------------------------------------------
    // Bind-pose local-to-parent translations come from the consumer skeleton
    // (SK_GestureSyncSource) so non-root bones publish their NATURAL rest
    // offsets (not zero) — without this every bone reaches its parent's
    // position and the rig collapses toward origin (head Z ~3cm instead of
    // ~150cm). The ref pose IS the MHR A-pose, so the same lookup also
    // provides the rest-rotation fallback used by the FingerPolicy override
    // below (identity != rest on the MHR rig — no separate rest constants).
    // For the root we ADD the MHR root delta on top of the bind translation.
    const FReferenceSkeleton* RefSkel = nullptr;
    {
        static USkeletalMesh* GestureSyncSrcMesh = LoadObject<USkeletalMesh>(nullptr,
            TEXT("/Game/Estuary/IKRetargeters/SK_GestureSyncSource.SK_GestureSyncSource"));
        if (GestureSyncSrcMesh)
        {
            RefSkel = &GestureSyncSrcMesh->GetRefSkeleton();
        }
    }

    // Resume blend arming (2026-06-10): first active publish after an interrupt
    // or HOLD gap. Snapshot the held pose as the blend SOURCE before the build
    // loop overwrites LastPublishedTransforms — the blend below slerps from this
    // FIXED snapshot toward the live stream pose over kResumeBlendSecs.
    if (bResumeBlendPending)
    {
        if (LastPublishedTransforms.Num() > 0)
        {
            ResumeBlendSourceTransforms = LastPublishedTransforms;
            ResumeBlendTimeAccumulated  = 0.0f;
            bResumeBlending             = true;
        }
        bResumeBlendPending = false;
    }

    LastPublishedTransforms.Reset(NumBones);
    for (int32 i = 0; i < NumBones; ++i)
    {
        // Wire name == skeleton bone name (generated MHR FBX) — no remap layer.
        const FName WireBoneName(*Interp.Bones[i].Name);

        const FQuat InQuat(
            Interp.Bones[i].Quat.X, Interp.Bones[i].Quat.Y,
            Interp.Bones[i].Quat.Z, Interp.Bones[i].Quat.W);
        // MHR→UE conversion — PROVISIONAL formula until the Phase 5 FK solve
        // (see EstuaryBodyFrameTransform.h).
        const FQuat OutQuat = EstuaryBodyFrameTransform::ToUEQuatFromMHR(InQuat);

        // Bind-local translation + scale from the source SK ref skeleton (the ref
        // pose IS the MHR A-pose). Wire names match the skeleton 1:1, so this
        // lookup hits for every bone on a healthy import; a persistent miss means
        // the FBX import is broken (Phase 5 import audit asserts names/order).
        //
        // SCALE (2026-07-03, found by the Task 25 PIE smoke): the Blender-built
        // FBX carries the m→cm conversion as a ×100 SCALE on the 'root' bone,
        // with child bind translations stored in METERS (the retired SMPL-X
        // import hid this on its extra armature node, which was never a
        // published subject bone; the MHR import strips that node, so 'root'
        // itself is the scale carrier). Publishing FTransforms with the default
        // scale of 1 clobbered root's ×100 in the LiveLink pose and collapsed
        // every child onto the root (all sockets within ~1 cm). Publish each
        // bone's REF-POSE scale so the skeleton's own unit convention survives
        // LiveLink regardless of import style.
        FVector BindLocalT = FVector::ZeroVector;
        FVector BindLocalScale = FVector::OneVector;
        if (RefSkel)
        {
            const int32 ChildIdx = RefSkel->FindBoneIndex(WireBoneName);
            if (ChildIdx != INDEX_NONE)
            {
                BindLocalT = RefSkel->GetRefBonePose()[ChildIdx].GetTranslation();
                BindLocalScale = RefSkel->GetRefBonePose()[ChildIdx].GetScale3D();
            }
        }

        FTransform Xform;
        Xform.SetRotation(OutQuat);
        Xform.SetScale3D(BindLocalScale);

        if (i == 0)
        {
            // Root: bind translation + MHR root-position delta (meters, agent
            // frame, pelvis-centred — Y oscillates ~0).
            Xform.SetLocation(BindLocalT
                              + EstuaryBodyFrameTransform::ToUEHipsFromMHR(Interp.HipsWorld));
        }
        else
        {
            Xform.SetLocation(BindLocalT);
        }
        LastPublishedTransforms.Add(Xform);
    }

    // -----------------------------------------------------------------
    // Resume blend (2026-06-10, replaces the interrupt ramp-to-bind-pose).
    // After an interrupt the server drops the dead utterance's frames and the
    // stream restarts from idle inference — the first frames can be far from
    // the held pose, reading as a snap. Slerp from the pose held at the seam
    // into the live stream. Transport-seam concealment only: past the blend
    // window the stream is rendered faithfully.
    // -----------------------------------------------------------------
    if (bResumeBlending)
    {
        ResumeBlendTimeAccumulated += DeltaTime;
        const float BlendAlpha = FMath::Clamp(
            ResumeBlendTimeAccumulated / kResumeBlendSecs, 0.0f, 1.0f);
        if (BlendAlpha >= 1.0f
            || ResumeBlendSourceTransforms.Num() != LastPublishedTransforms.Num())
        {
            // Blend complete — or the bone schema changed mid-blend (snap
            // rather than mis-blend across mismatched layouts).
            bResumeBlending = false;
            ResumeBlendTimeAccumulated = 0.0f;
            ResumeBlendSourceTransforms.Reset();
        }
        else
        {
            for (int32 i = 0; i < LastPublishedTransforms.Num(); ++i)
            {
                LastPublishedTransforms[i].SetRotation(FQuat::Slerp(
                    ResumeBlendSourceTransforms[i].GetRotation(),
                    LastPublishedTransforms[i].GetRotation(), BlendAlpha));
            }
            // Hips is the only bone carrying live translation (the rest are
            // constant bind-local offsets identical in both arrays).
            LastPublishedTransforms[0].SetLocation(FMath::Lerp(
                ResumeBlendSourceTransforms[0].GetLocation(),
                LastPublishedTransforms[0].GetLocation(), BlendAlpha));
        }
    }

    // -----------------------------------------------------------------
    // FingerPolicy override.
    // Non-finger MHR63 bones always Driven by GestureSync frames; finger bones
    // (the interleaved sets 23-39 ∪ 44-60 — see EstuaryMHR::IsFingerIndex) follow
    // the policy. Driven (default) plays GestureSync frames pass-through.
    // The hold-last-pose path above already covers no-frames behavior for
    // all bones — this branch ONLY overrides fingers when GestureSync frames are
    // flowing and the operator has selected a non-Driven policy.
    // -----------------------------------------------------------------
    if (FingerPolicy != EEstuaryFingerPolicy::Driven)
    {
        // MHR63 fingers are NOT a contiguous tail: right fingers 23-39 and left
        // fingers 44-60 interleave with clavicles/neck/head, so the old
        // FingerStartIdx=22 split is invalid. Membership comes from the generated
        // EstuaryMHR::IsFingerIndex (kRightFingerFirst/Last + kLeftFingerFirst/Last).
        for (int32 i = 0; i < LastPublishedTransforms.Num() && i < CachedBoneNames.Num(); ++i)
        {
            if (!EstuaryMHR::IsFingerIndex(i)) { continue; }

            // Rest rotation for this finger bone comes from the source SK ref
            // skeleton — the ref pose IS the MHR A-pose, and identity != rest on
            // the MHR rig (non-identity binds). Identity fallback only when the
            // SK asset failed to load (the BeginPlay warning path already fired).
            FQuat RestQ = FQuat::Identity;
            if (RefSkel)
            {
                const int32 BoneIdx = RefSkel->FindBoneIndex(CachedBoneNames[i]);
                if (BoneIdx != INDEX_NONE)
                {
                    RestQ = RefSkel->GetRefBonePose()[BoneIdx].GetRotation();
                }
            }

            switch (FingerPolicy)
            {
            case EEstuaryFingerPolicy::BindPose:
            {
                // Force finger to the MHR A-pose rest every frame. Overrides the
                // GestureSync-driven finger transform written above.
                LastPublishedTransforms[i].SetRotation(RestQ);
                break;
            }
            case EEstuaryFingerPolicy::IdleBreath:
            {
                // v2.0 stub: behaves identically to Driven (no override).
                // TODO (future): additive subtle finger idle motion overlaid on
                // the GestureSync-driven fingers.
                break;
            }
            case EEstuaryFingerPolicy::Statue:
            {
                // Freeze fingers indefinitely. v2.0 simplified semantics:
                // forces fingers to the A-pose rest (TRUE per-utterance freeze
                // deferred to v2.x). This is the explicit
                // "broken state" indicator for QA — never the default;
                // surfaces "frames stopped flowing but client thought they
                // were live" as a visible artifact rather than silent freeze.
                LastPublishedTransforms[i].SetRotation(RestQ);
                break;
            }
            case EEstuaryFingerPolicy::Driven:
            default:
                // Unreachable (outer if guards Driven case). No-op for
                // forward-compat with future enum extensions.
                break;
            }
        }
    }

    // -----------------------------------------------------------------
    // Publish to Live Link via bridge.
    // -----------------------------------------------------------------
    Pub->PublishFrame(SubjectName, LastPublishedTransforms,
                      FApp::GetCurrentTime(), CurrentMessageId);

    // -----------------------------------------------------------------
    // Drift telemetry (mirror face).
    // Drift > kDriftThresholdMs is logged at most once per second so the
    // diagnostic is visible without flooding the log at 60 Hz tick rate.
    // The exceedance count still increments per tick for accurate stat
    // reporting via GetDriftExceededCount.
    // -----------------------------------------------------------------
    LastDriftMs = Buffer.GetDriftMs(RelativeAudioTime);
    if (FMath::Abs(LastDriftMs) > kDriftThresholdMs)
    {
        ++DriftExceededCount;
        const double NowSeconds = FPlatformTime::Seconds();
        constexpr double kDriftWarnIntervalSeconds = 1.0;
        if (NowSeconds - LastDriftWarnAtSeconds >= kDriftWarnIntervalSeconds)
        {
            LastDriftWarnAtSeconds = NowSeconds;
            UE_LOG(LogEstuary, Verbose,
                TEXT("[BodyAnimReceiver] drift=%.1fms (count=%d, throttled to 1Hz). "
                     "AudioTimeSec=%.3f RelativeTime=%.3f msg=%s. Tune "
                     "FirstFrameLatencyCompensationMs in the receiver Details panel "
                     "if drift is consistently positive."),
                LastDriftMs, DriftExceededCount, AudioTimeSec, RelativeAudioTime,
                *CurrentMessageId);
        }
    }
}

// ---------------------------------------------------------------------------
// HandleBotPoseEvent — mirror face HandleBotAnimationEvent.
// Per-utterance reset on MessageId change; push frame to buffer.
// Replaces scaffold's diagnostic-capture body verbatim.
// ---------------------------------------------------------------------------
void UEstuaryBodyAnimationReceiver::HandleBotPoseEvent(const FEstuaryBotPose& Pose)
{
    // Subsystem guarantees game-thread dispatch (HandleBotPose -> CharComp -> here).
    check(IsInGameThread());
    if (!bAnimationEnabled) { return; }

    // Drop in-flight frames of the interrupted utterance that land AFTER the
    // interrupt ack — they'd re-fill the just-cleared buffer and resurrect the
    // dead utterance (server-side filtering covers most of this; this is the
    // contract-mandated client-side defense). A frame with any OTHER id ends
    // the filter (idle stream or next utterance has taken over).
    if (!InterruptedMessageId.IsEmpty())
    {
        if (Pose.MessageId == InterruptedMessageId)
        {
            return;
        }
        InterruptedMessageId.Empty();
    }

    // Hard count gate: the MHR63 wire is exactly 63 bones — the legacy 22/52
    // tolerance is DELETED (SMPL-X retired wholesale; mixed versions must fail
    // LOUDLY, spec §8 — Error at 1 Hz, not Verbose). Terminators are filtered
    // by the subsystem before dispatch — receiver never sees them — but the
    // buffer's Push silently ignores them too.
    if (!Pose.IsTerminator() && Pose.Bones.Num() != EstuaryMHR::kNumBones)
    {
        const double NowSeconds = FPlatformTime::Seconds();
        constexpr double kBoneCountErrorIntervalSeconds = 1.0;
        if (NowSeconds - LastBoneCountErrorLogSeconds >= kBoneCountErrorIntervalSeconds)
        {
            LastBoneCountErrorLogSeconds = NowSeconds;
            UE_LOG(LogEstuary, Error,
                TEXT("[BodyAnimReceiver] Dropping bot_pose frame with unexpected bone count: "
                     "got=%d expected=%d (msg=%s seq=%d). Server rig and client are out of "
                     "sync — check session_info animation_capabilities.body.rig."),
                Pose.Bones.Num(), EstuaryMHR::kNumBones, *Pose.MessageId, Pose.FrameIndex);
        }
        return;
    }

    // New utterance: capture the audio clock as this message's time-base anchor
    // and discard any leftover frames from the prior message. Same rationale as
    // face — see EstuaryAnimationReceiver.cpp HandleBotAnimationEvent for the
    // first-frame-latency-compensation derivation; body uses the same default
    // (150 ms) so face + body stay in lockstep.
    const float FirstFrameLatencyCompSec = FirstFrameLatencyCompensationMs / 1000.0f;
    if (!Pose.MessageId.IsEmpty() && Pose.MessageId != CurrentMessageId)
    {
        float NowAudio = 0.0f;
        if (UEstuaryAudioPlayerComponent* AudioComp = WeakAudioComp.Get())
        {
            // SAME clock as face (cross-modality drift impossible by construction).
            NowAudio = AudioComp->GetPlaybackTimeSeconds();
        }
        Buffer.Clear();
        CurrentMessageId   = Pose.MessageId;
        MessageAudioOffset = NowAudio - FirstFrameLatencyCompSec;
        // Worker emits a continuous silence-fill "idle" utterance between real bot
        // utterances so GestureSync always streams listening motion. Idle ids carry
        // the reserved PREFIX "__sarah_idle__" with a per-resume unique suffix
        // ("__sarah_idle__#7") so this message-id-change reset fires on every idle
        // resume — equality matching would silently fall back to the frozen
        // bot-audio clock. (Some legacy builds used an "idle:N" prefix.) The
        // bot-audio clock freezes between speech (GetPlaybackTimeSeconds plateaus
        // once the TTS queue drains), so idle frames are driven off the wall clock;
        // real (TTS) utterances keep audio-clock anchoring for A/V sync.
        bUseWallClockForCurrentMessage =
            Pose.MessageId.StartsWith(TEXT("idle:")) ||
            Pose.MessageId.StartsWith(TEXT("__sarah_idle__"));
        MessageWallClockOffset = FApp::GetCurrentTime()
                                 - static_cast<double>(FirstFrameLatencyCompSec);
        // Force the publisher to re-EnsureSubject on this utterance's first tick so
        // the schema is re-established across message rollovers.
        bSchemaEnsuredForCurrentMessage = false;
        // Reset drift-warn throttle so the FIRST drift warning of a new utterance fires
        // immediately instead of waiting for the 1 Hz window.
        LastDriftWarnAtSeconds = 0.0;
        // Arm the resume blend for the message handover (2026-06-10). The playhead
        // is per-message (Buffer.Clear() + fresh anchor) and under the adaptive
        // slew (EstuaryPlayheadMath.h) it trails the newest frame by ~kTargetLagSec,
        // so the last displayed pose of the OLD message and the first sampled pose
        // of the NEW one are up to ~0.4 s of motion apart — a visible snap at every
        // idle↔speech handover. Arming here makes the first active publish snapshot
        // the held pose and slerp into the new stream over kResumeBlendSecs
        // (transport-seam concealment, same mechanism as the HOLD/interrupt paths).
        // If a blend is already in flight (e.g. armed by HOLD post-interrupt),
        // re-arming just re-snapshots from the current blended pose — continuous,
        // no snap.
        bResumeBlendPending = true;
        UE_LOG(LogEstuary, Log,
            TEXT("[BodyAnimReceiver] New utterance msg=%s — anchor=%.3fs "
                 "(NowAudio=%.3f - latencyComp=%.3f). Tune via "
                 "FirstFrameLatencyCompensationMs UPROPERTY."),
            *CurrentMessageId, MessageAudioOffset, NowAudio, FirstFrameLatencyCompSec);
    }

    // [BodyDiag] count real (non-terminator) frames for the supply-rate telemetry
    // (recvFps in the TickComponent [BodyDiag] line).
    if (!Pose.IsTerminator()) { ++DiagFramesRecvWindow; }

    // Push to buffer. TEstuaryAnimationFrameBuffer<FEstuaryBotPose>::Push silently
    // ignores terminator frames (defense-in-depth — subsystem already filters).
    Buffer.Push(Pose);
}

// ---------------------------------------------------------------------------
// HandleInterruptAckedEvent — hold-last-pose (2026-06-10).
// Clears buffer (drops the interrupted utterance's in-flight frames) and arms
// the resume blend. The tick's HOLD path re-publishes the last pose until the
// idle pump's stream arrives (~0.3-1 s); the active path then slerps from the
// held pose into the live stream over kResumeBlendSecs.
//
// This REPLACES the 500 ms ramp-to-bind-pose (the old decay path): the shipped
// "bind pose" was all-identity quats, and identity local rotations are the
// T-POSE under the VRM identity-bind convention — every barge-in
// visibly snapped the character to T-pose for the length of the post-interrupt
// frame blackout. The model's idle stream is the only correct source of rest
// motion; the client must not invent poses.
// ---------------------------------------------------------------------------
void UEstuaryBodyAnimationReceiver::HandleInterruptAckedEvent(const FEstuaryInterruptPayload& Payload)
{
    check(IsInGameThread());

    // Record the id so late in-flight frames of the interrupted utterance get
    // dropped in HandleBotPoseEvent instead of re-filling the buffer.
    InterruptedMessageId = Payload.MessageId;

    // Only nuke the buffer when the ack names the utterance we're playing (or
    // is anonymous — defensive). The server's Deepgram path acks EVERY user
    // speech start, including during bot silence when the buffer holds valid
    // idle-stream frames; clearing those caused a pointless hold-and-blend
    // stutter on each user utterance. Mirrors the audio player's
    // FlushAllForMessage message-id gate.
    const bool bAffectsCurrentMessage =
        Payload.MessageId.IsEmpty() || Payload.MessageId == CurrentMessageId;
    if (bAffectsCurrentMessage)
    {
        // Drop pending frames — the gateway/worker stop the interrupted
        // message's stream server-side; anything buffered here is already dead.
        Buffer.Clear();
        bResumeBlendPending = true;
    }

    UE_LOG(LogEstuary, Log,
        TEXT("[BodyAnimReceiver] Interrupt acked (message_id=%s, current=%s, "
             "cleared=%d). Holding last pose (%d transforms) until the stream "
             "resumes."),
        *Payload.MessageId,
        CurrentMessageId.IsEmpty() ? TEXT("<none>") : *CurrentMessageId,
        bAffectsCurrentMessage ? 1 : 0,
        LastPublishedTransforms.Num());
}
