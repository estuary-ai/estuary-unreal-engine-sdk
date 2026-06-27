// EstuaryBodyAnimationReceiver.cpp — UEstuaryBodyAnimationReceiver production impl.
//
// Pitfall 1:  All UObject touches on the game thread — TickComponent + BeginPlay guarantee this.
// Pitfall 2:  HandleBotPoseEvent / HandleInterruptAckedEvent are UFUNCTION() (see header).
// Pitfall 3:  TWeakObjectPtr cached in BeginPlay; stale-pointer defense via .Get() null-checks.
// Pitfall 6:  LastPublishedTransforms.Reserve(52) at BeginPlay; FQuat::Slerp + FVector::Lerp
//             don't allocate; DataTable lookup is O(1).
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
#include "EstuaryBodyBoneRemap.h"
#include "EstuaryPlayheadMath.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"

namespace
{
    // 2026-05-05: GestureSync server (Blender-anatomical wire names per
    // the body model's canonical bone order — `hips`,
    // `leftUpperArm`, `leftLowerLeg`, etc.) → SK_GestureSyncSource skeleton
    // (SMPL-X canonical names — `pelvis`, `left_shoulder`, `left_knee`, etc.).
    // SK_GestureSyncSource is now the OFFICIAL SMPL-X male FBX from SMPL-X's Unity
    // demo project (Models/smplx-male.fbx) — proper anatomical bind pose,
    // proper heat-equilibrium skin weights, 22 body + 30 finger bones, plus
    // 3 face bones we don't drive (jaw, left_eye_smplhf, right_eye_smplhf).
    //
    // Earlier doc-comments referenced VRM-canonical wire names (`leftArm`,
    // `leftForeArm`, `leftUpLeg`, `leftLeg`) — that turned out to be wrong;
    // verifying GestureSync's CANONICAL_22_BONE_NAMES showed the wire is and always
    // has been Blender-anatomical. The previous remap entries with VRM keys
    // were dead code.
    //
    // Mapping covers the full 52 GestureSync wire bones (22 body + 30 fingers).
    // Bones that are identity-named on both sides (`neck`, `head`) are listed
    // for documentation; bones not in the map (none in current schema) pass
    // through unchanged.
    static FName RemapGestureSyncToSkeletonBoneName(const FName& InGestureSyncName)
    {
        static const TMap<FName, FName> Map = {
            // Body 22 bones (CANONICAL_22_BONE_NAMES → SMPL-X canonical)
            { TEXT("hips"),           TEXT("pelvis")         },
            { TEXT("leftUpperLeg"),   TEXT("left_hip")       },
            { TEXT("rightUpperLeg"),  TEXT("right_hip")      },
            { TEXT("spine"),          TEXT("spine1")         },
            { TEXT("leftLowerLeg"),   TEXT("left_knee")      },
            { TEXT("rightLowerLeg"),  TEXT("right_knee")     },
            { TEXT("chest"),          TEXT("spine2")         },
            { TEXT("leftFoot"),       TEXT("left_ankle")     },
            { TEXT("rightFoot"),      TEXT("right_ankle")    },
            { TEXT("upperChest"),     TEXT("spine3")         },
            { TEXT("leftToes"),       TEXT("left_foot")      },
            { TEXT("rightToes"),      TEXT("right_foot")     },
            { TEXT("neck"),           TEXT("neck")           },
            { TEXT("leftShoulder"),   TEXT("left_collar")    },
            { TEXT("rightShoulder"),  TEXT("right_collar")   },
            { TEXT("head"),           TEXT("head")           },
            { TEXT("leftUpperArm"),   TEXT("left_shoulder")  },
            { TEXT("rightUpperArm"),  TEXT("right_shoulder") },
            { TEXT("leftLowerArm"),   TEXT("left_elbow")     },
            { TEXT("rightLowerArm"),  TEXT("right_elbow")    },
            { TEXT("leftHand"),       TEXT("left_wrist")     },
            { TEXT("rightHand"),      TEXT("right_wrist")    },
            // Fingers 30 (camelCase → snake_case; left + right × 5 fingers × 3 phalanges)
            { TEXT("leftIndex1"),    TEXT("left_index1")    },
            { TEXT("leftIndex2"),    TEXT("left_index2")    },
            { TEXT("leftIndex3"),    TEXT("left_index3")    },
            { TEXT("leftMiddle1"),   TEXT("left_middle1")   },
            { TEXT("leftMiddle2"),   TEXT("left_middle2")   },
            { TEXT("leftMiddle3"),   TEXT("left_middle3")   },
            { TEXT("leftPinky1"),    TEXT("left_pinky1")    },
            { TEXT("leftPinky2"),    TEXT("left_pinky2")    },
            { TEXT("leftPinky3"),    TEXT("left_pinky3")    },
            { TEXT("leftRing1"),     TEXT("left_ring1")     },
            { TEXT("leftRing2"),     TEXT("left_ring2")     },
            { TEXT("leftRing3"),     TEXT("left_ring3")     },
            { TEXT("leftThumb1"),    TEXT("left_thumb1")    },
            { TEXT("leftThumb2"),    TEXT("left_thumb2")    },
            { TEXT("leftThumb3"),    TEXT("left_thumb3")    },
            { TEXT("rightIndex1"),   TEXT("right_index1")   },
            { TEXT("rightIndex2"),   TEXT("right_index2")   },
            { TEXT("rightIndex3"),   TEXT("right_index3")   },
            { TEXT("rightMiddle1"),  TEXT("right_middle1")  },
            { TEXT("rightMiddle2"),  TEXT("right_middle2")  },
            { TEXT("rightMiddle3"),  TEXT("right_middle3")  },
            { TEXT("rightPinky1"),   TEXT("right_pinky1")   },
            { TEXT("rightPinky2"),   TEXT("right_pinky2")   },
            { TEXT("rightPinky3"),   TEXT("right_pinky3")   },
            { TEXT("rightRing1"),    TEXT("right_ring1")    },
            { TEXT("rightRing2"),    TEXT("right_ring2")    },
            { TEXT("rightRing3"),    TEXT("right_ring3")    },
            { TEXT("rightThumb1"),   TEXT("right_thumb1")   },
            { TEXT("rightThumb2"),   TEXT("right_thumb2")   },
            { TEXT("rightThumb3"),   TEXT("right_thumb3")   },
        };
        if (const FName* Mapped = Map.Find(InGestureSyncName))
        {
            return *Mapped;
        }
        return InGestureSyncName;
    }
}

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
#include "Engine/DataTable.h"
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
    LastPublishedTransforms.Reserve(52);
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

    // 2026-06-10: the bundled metahuman_common_v1.json bind-pose load was removed
    // along with the interrupt ramp-to-bind-pose. The file shipped all-identity
    // quats — the T-pose under the VRM identity-bind convention — AND the
    // cache was keyed by GestureSync wire names while the ramp looked it up by
    // post-remap skeleton names, so the lookup missed to FQuat::Identity anyway.
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
// Subject naming helper — "<characterId>_Body".
// Single-place enforcement of the body subject convention. Face stays bare-id
// (so v1.0 face ABPs continue to work); body always suffixes "_Body" so face
// and body subjects can coexist on the same actor.
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

    FEstuaryLiveLinkBridge& Bridge = Sub->GetLiveLinkBridge();
    if (!Bridge.IsBodyAvailable()) { return; }

    IEstuaryBodyLiveLinkPublisher* Pub = Bridge.GetBodyPublisher();
    if (!Pub) { return; }

    // Resolve subject FName ("<characterId>_Body").
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
    // J=52 fixed at public surface; receiver iterates Bones.Num() defensively.
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
        // 2026-05-03: Hardcoded GestureSync 52-bone parent topology (matches
        // DT_GestureSyncBoneRemap_Identity row order + the body model's canonical bone order
        // CANONICAL_52_BONE_NAMES). A prior placeholder ("everything child of root")
        // corrupted the LiveLink skeleton hierarchy and broke parent-relative pose
        // evaluation in LiveLinkPose, so matching-named bones appeared static.
        //
        // 2026-05-05 cleanup (punchlist item 1): comments use Blender-anatomical
        // names ONLY — same name space the receiver publishes (post
        // RemapGestureSyncToSkeletonBoneName for body 0-21; pass-through for fingers 22-51).
        // No MetaHuman bone names appear here; IKR_GestureSyncToMetaHuman handles cross-skel
        // alignment downstream.
        static const int32 kGestureSyncBoneParents[52] = {
            -1,  // 0  hips
             0,  // 1  leftUpperLeg          -> hips
             0,  // 2  rightUpperLeg         -> hips
             0,  // 3  spine                 -> hips
             1,  // 4  leftLowerLeg          -> leftUpperLeg
             2,  // 5  rightLowerLeg         -> rightUpperLeg
             3,  // 6  chest                 -> spine
             4,  // 7  leftFoot              -> leftLowerLeg
             5,  // 8  rightFoot             -> rightLowerLeg
             6,  // 9  upperChest            -> chest
             7,  // 10 leftToes              -> leftFoot
             8,  // 11 rightToes             -> rightFoot
             9,  // 12 neck                  -> upperChest
             9,  // 13 leftShoulder          -> upperChest
             9,  // 14 rightShoulder         -> upperChest
            12,  // 15 head                  -> neck
            13,  // 16 leftUpperArm          -> leftShoulder
            14,  // 17 rightUpperArm         -> rightShoulder
            16,  // 18 leftLowerArm          -> leftUpperArm
            17,  // 19 rightLowerArm         -> rightUpperArm
            18,  // 20 leftHand              -> leftLowerArm
            19,  // 21 rightHand             -> rightLowerArm
            20, 22, 23,  // 22-24 leftIndex1..3   (root proximal under leftHand)
            20, 25, 26,  // 25-27 leftMiddle1..3
            20, 28, 29,  // 28-30 leftPinky1..3
            20, 31, 32,  // 31-33 leftRing1..3
            20, 34, 35,  // 34-36 leftThumb1..3
            21, 37, 38,  // 37-39 rightIndex1..3
            21, 40, 41,  // 40-42 rightMiddle1..3
            21, 43, 44,  // 43-45 rightPinky1..3
            21, 46, 47,  // 46-48 rightRing1..3
            21, 49, 50,  // 49-51 rightThumb1..3
        };

        CachedBoneNames.Reset(NumBones);
        CachedBoneParents.Reset(NumBones);
        for (int32 i = 0; i < NumBones; ++i)
        {
            // 2026-05-04 IKR refactor: publish GestureSync bone names with a thin VRM→Blender
            // skeleton-name remap so SK_GestureSyncSource (which was FBX-exported with Blender
            // anatomical names) actually receives the upper/lower arm + leg rotations.
            // Without the remap LiveLink silently drops 8 bones whose names don't exist
            // on the source skeleton.
            CachedBoneNames.Add(RemapGestureSyncToSkeletonBoneName(FName(*Interp.Bones[i].Name)));
            const int32 ParentIdx = (i < 52) ? kGestureSyncBoneParents[i] : -1;
            CachedBoneParents.Add(ParentIdx);
        }
        Pub->EnsureSubject(SubjectName, CachedBoneNames, CachedBoneParents);
        bSchemaEnsuredForCurrentMessage = true;
    }

    // -----------------------------------------------------------------
    // SMPL-X→UE conversion + DataTable bone-name remap into LastPublishedTransforms.
    // EstuaryBodyFrameTransform::ToUEQuatFromGestureSync(quat, RollOffsetDeg) handles the
    // global axis swap + sign flip + handedness + per-bone roll.
    // EstuaryBodyFrameTransform::ToUEHipsFromGestureSync(meters) handles × 100 m→cm
    // hips conversion (single source of truth).
    // -----------------------------------------------------------------
    // 2026-05-03: Read consumer skeleton's bind-pose local-to-parent translations
    // so non-hips bones publish their NATURAL rest-pose offsets (not zero).
    // Without this, every bone reaches its parent's position and the rig collapses
    // toward origin (head Z ~3cm instead of ~150cm). For hips/pelvis we ADD the
    // GestureSync hips world delta on top of the bind-pose translation so GestureSync-driven
    // hip motion still translates the root, but the natural pelvis-vs-root offset
    // is preserved (otherwise pelvis snaps to 0 when GestureSync hips delta is zero).
    //
    // 2026-05-05 IKR refactor: consumer is SK_GestureSyncSource (SMPL-X canonical),
    // NOT MH directly. The receiver publishes SMPL-X-named bones (`pelvis`,
    // `left_shoulder`, ...) — these names don't exist on the MH skeleton, so
    // the legacy MH lookup missed and translation came out 0 → SK_GestureSyncSource
    // collapsed → IKR retargeted a collapsed pose to MH = MH stuck at A-pose.
    // Look up bind from SK_GestureSyncSource (the LiveLink consumer mesh) instead.
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
        // 2026-05-04: VRM-canonical GestureSync server name → SK_GestureSyncSource skeleton (Blender
        // anatomical) name remap. Mirrors the same remap applied during schema build above
        // so per-frame transforms align with the schema bone names LiveLink already received.
        const FName GestureSyncBoneName = RemapGestureSyncToSkeletonBoneName(FName(*Interp.Bones[i].Name));

        const FQuat InQuat(
            Interp.Bones[i].Quat.X, Interp.Bones[i].Quat.Y,
            Interp.Bones[i].Quat.Z, Interp.Bones[i].Quat.W);
        // World-frame conversion (locked, 6-pose math gate).
        const FQuat OutQuat = EstuaryBodyFrameTransform::ToUEQuatFromGestureSync(InQuat, /*RollOffsetDeg=*/0.0f);

        // BindLocalT lookup against the resolved RefSkel (typically the MH body skel)
        // returns INDEX_NONE for GestureSync bone names → BindLocalT stays zero, which is
        // fine: the IKR's Retarget Pose From Mesh derives target translations from
        // chain proportions on the target rig, not from per-frame translation values.
        FVector BindLocalT = FVector::ZeroVector;
        if (RefSkel)
        {
            const int32 ChildIdx = RefSkel->FindBoneIndex(GestureSyncBoneName);
            if (ChildIdx != INDEX_NONE)
            {
                BindLocalT = RefSkel->GetRefBonePose()[ChildIdx].GetTranslation();
            }
        }

        FTransform Xform;
        Xform.SetRotation(OutQuat);

        if (i == 0)
        {
            // Pelvis: bind translation (typically zero for GestureSync names) + GestureSync hip delta.
            Xform.SetLocation(BindLocalT
                              + EstuaryBodyFrameTransform::ToUEHipsFromGestureSync(Interp.HipsWorld));
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
    // Body bones (0..21) always Driven by GestureSync frames; fingers (22..51)
    // follow the policy. Driven (default) plays GestureSync frames pass-through.
    // The hold-last-pose path above already covers no-frames behavior for
    // all bones — this branch ONLY overrides fingers when GestureSync frames are
    // flowing and the operator has selected a non-Driven policy.
    // -----------------------------------------------------------------
    if (FingerPolicy != EEstuaryFingerPolicy::Driven)
    {
        const int32 FingerStartIdx = 22;
        for (int32 i = FingerStartIdx; i < LastPublishedTransforms.Num() && i < CachedBoneNames.Num(); ++i)
        {
            switch (FingerPolicy)
            {
            case EEstuaryFingerPolicy::BindPose:
            {
                // Force finger to rest pose every frame. Overrides the
                // GestureSync-driven finger transform written above. Identity local
                // rotation == SK_GestureSyncSource bind for fingers (VRM normalized
                // semantics) — this matches what the removed bind-pose JSON
                // resolved to anyway (all-identity quats + key-mismatch miss).
                LastPublishedTransforms[i].SetRotation(FQuat::Identity);
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
                // forces fingers to bind pose (TRUE per-utterance freeze
                // deferred to v2.x). This is the explicit
                // "broken state" indicator for QA — never the default;
                // surfaces "frames stopped flowing but client thought they
                // were live" as a visible artifact rather than silent freeze.
                LastPublishedTransforms[i].SetRotation(FQuat::Identity);
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

    // Defensive length check (hard-cut to J=52, but
    // length-flex defense-in-depth at the receiver tolerates the legacy J=22
    // wire if some hardware is somehow still on the legacy stream). Anything
    // else (other than the terminator) gets logged Verbose and dropped.
    // Terminators are filtered by the subsystem before dispatch — receiver
    // never sees them — but the buffer's Push silently ignores them too.
    if (!Pose.IsTerminator() && Pose.Bones.Num() != 52 && Pose.Bones.Num() != 22)
    {
        UE_LOG(LogEstuary, Verbose,
            TEXT("[BodyAnimReceiver] Dropping bot_pose frame with unexpected bone count: "
                 "got=%d expected=22 or 52 (msg=%s seq=%d)."),
            Pose.Bones.Num(), *Pose.MessageId, Pose.FrameIndex);
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
        // utterances so GestureSync always streams listening motion. Its message_id is the
        // sentinel "__sarah_idle__" (the backend idle-frame message id; some
        // builds also used an "idle:N" prefix). The bot-audio clock freezes between
        // speech (GetPlaybackTimeSeconds plateaus once the TTS queue drains), so idle
        // frames are driven off the wall clock; real (TTS) utterances keep audio-clock
        // anchoring for A/V sync.
        bUseWallClockForCurrentMessage =
            Pose.MessageId.StartsWith(TEXT("idle:")) ||
            Pose.MessageId == TEXT("__sarah_idle__");
        MessageWallClockOffset = FApp::GetCurrentTime()
                                 - static_cast<double>(FirstFrameLatencyCompSec);
        // Force the publisher to re-EnsureSubject on this utterance's first tick so
        // any BoneRemapAsset change re-establishes the schema.
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
