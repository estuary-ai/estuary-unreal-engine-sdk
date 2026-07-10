// EstuaryBodyAnimationReceiver.h — Production body Live Link receiver.
//
// REPLACES the earlier first-pass scaffold. Class name UEstuaryBodyAnimationReceiver
// is preserved verbatim (no migration friction for in-flight Blueprint references);
// the diagnostic API surface from the scaffold is REMOVED — that was wire-test
// scaffolding and should not bleed into the release as production API.
//
// Pitfall 23: #pragma once, IWYU only, no CoreMinimal.h, no Engine.h.
// Pitfall 7:  ZERO LiveLink/* includes — receiver consumes IEstuaryBodyLiveLinkPublisher
//             plain C++ interface via FEstuaryLiveLinkBridge service-locator.
// Pitfall 8:  HandleBotPoseEvent / HandleInterruptAckedEvent are UFUNCTION() for AddDynamic.
// Pitfall 25: LiveLinkPose holds last-pushed frame indefinitely. The body receiver
//             keeps the subject fresh by actively re-publishing the LAST pose while
//             the buffer is empty (hold-last-pose), then slerping into the resuming
//             stream over kResumeBlendSecs. 2026-06-10: this REPLACED the 500 ms
//             ramp-to-bind-pose — identity quats are the T-POSE under the VRM
//             identity-bind convention, so the old decay snapped the character to
//             T-pose on every barge-in. The model's idle stream is the only correct
//             source of rest motion; the client must not invent poses.
// Subject naming: `<characterId>_Body_GestureSync` — face stays bare-id; body suffixes.
//             Single-place enforcement in ResolveSubjectName().
// Audio clock: Both face and body receivers read SAME WeakAudioComp->GetPlaybackTimeSeconds()
//             — cross-modality drift impossible by construction (sub-100 ms drift target
//             trivially satisfied: there is no drift to measure).
// Bone count: J=63 (MHR63 wire) — hard ingest gate (==EstuaryMHR::kNumBones), no
//             legacy 22/52 tolerance; plus a session rig gate on
//             session_info.animation_capabilities.body.rig ("mhr63").

#pragma once

#include "CoreTypes.h"
#include "UObject/ObjectMacros.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/NameTypes.h"
#include "Components/ActorComponent.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Math/Quat.h"
#include "Math/Transform.h"
#include "EstuaryTypes.h"                  // FEstuaryBotPose, FEstuaryInterruptPayload, EEstuaryFingerPolicy
#include "EstuaryAnimationFrameBuffer.h"   // TEstuaryAnimationFrameBuffer template

#include "EstuaryBodyAnimationReceiver.generated.h"

// Forward declarations (Pitfall 23 — minimize public-header include footprint).
class UEstuaryCharacterComponent;
class UEstuaryAudioPlayerComponent;
class USkeletalMeshComponent;

/**
 * UEstuaryBodyAnimationReceiver — production body-animation receiver.
 *
 * Replaces the earlier first-pass scaffold; class name preserved so existing
 * Blueprint references continue to work. The diagnostic API surface from the scaffold
 * was wire-test scaffolding only and is removed in this revision.
 *
 * Behavior (mirrors UEstuaryAnimationReceiver face structure):
 *   - Auto-binds to sibling UEstuaryCharacterComponent's OnBotPose + OnInterruptAcked
 *     delegates at BeginPlay.
 *   - Buffers frames in TEstuaryAnimationFrameBuffer<FEstuaryBotPose>.
 *   - Each tick:
 *       1) Checks the session rig gate (session_info animation_capabilities.body.rig
 *          must be "mhr63" — mismatch disables the component with ONE Error log).
 *       2) Resolves FEstuaryLiveLinkBridge::GetBodyPublisher() (no-op if unavailable).
 *       3) Reads WeakAudioComp->GetPlaybackTimeSeconds() (shared cross-modality clock).
 *       4) Interpolates against audio time (per-bone FQuat::Slerp + per-root FVector::Lerp).
 *       5) Applies MHR→UE coord conversion via EstuaryBodyFrameTransform::ToUEQuatFromMHR
 *          (PROVISIONAL formula until the Phase 5 FK solve — see that header).
 *       6) Publishes 63 FTransforms via Bridge.GetBodyPublisher()->PublishFrame — wire
 *          bone names ARE the SK_GestureSyncSource skeleton names (generated MHR FBX),
 *          so there is no name-remap layer; topology from generated EstuaryMHRRig.h.
 *
 * Subject naming: <characterId>_Body_GestureSync. ResolveSubjectName helper appends
 * "_Body_GestureSync" suffix to the bare CharacterId from the sibling UEstuaryCharacterComponent.
 *
 * Interrupt behavior (replaces the earlier ramp-to-bind-pose): on
 * interrupt the buffer is cleared and the receiver HOLDS the last published pose
 * (re-published each tick — Pitfall 25 stale-frame cache stays warm) until the
 * server's idle pump resumes the stream, then slerps from the held pose into the
 * live stream over kResumeBlendSecs (transport-seam concealment only).
 *
 * NOTE: the FingerPolicy override branches are implemented (see TickComponent).
 * `bAnimationEnabled` is still a per-receiver UPROPERTY only (default true) — it
 * is not yet wired to the project-wide `UEstuaryDeveloperSettings::bEnableBodyAnimation` global.
 */
UCLASS(ClassGroup=(Estuary),
       meta=(BlueprintSpawnableComponent,
             DisplayName="Estuary Body Animation Receiver",
             ToolTip="Production body-animation receiver. Buffers bot_pose frames (MHR 63-joint GestureSync wire), interpolates against the audio clock, applies the MHR→UE conversion, and publishes 63 FTransforms (wire bone names) via the body Live Link bridge. Drop on AEstuaryCharacter alongside UEstuaryCharacterComponent + UEstuaryAudioPlayerComponent."))
class ESTUARY_API UEstuaryBodyAnimationReceiver : public UActorComponent
{
    GENERATED_BODY()

public:
    UEstuaryBodyAnimationReceiver();

    // -----------------------------------------------------------------------
    // UActorComponent overrides
    // -----------------------------------------------------------------------

    /** Auto-wires to sibling UEstuaryCharacterComponent + UEstuaryAudioPlayerComponent
     *  on the same actor. Binds OnBotPose + OnInterruptAcked. */
    virtual void BeginPlay() override;

    /** Removes OnBotPose + OnInterruptAcked dynamic bindings; clears Buffer. */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /** Audio-clock-driven interpolate + convert + remap + publish path.
     *  Resolves the bridge each tick (no-op if EstuaryLiveLink module hasn't
     *  registered the body publisher yet). */
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

    // -----------------------------------------------------------------------
    // User-configurable properties
    // -----------------------------------------------------------------------

    /** Finger driving policy. Default `Driven` (live GestureSync frames).
     *  BindPose / IdleBreath / Statue alternatives are wired in a later revision. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Estuary|BodyAnimation",
              meta=(DisplayName="Finger Policy",
                    ToolTip="Finger driving policy. Driven (default; GestureSync frames). BindPose / IdleBreath / Statue handled in a later revision."))
    EEstuaryFingerPolicy FingerPolicy = EEstuaryFingerPolicy::Driven;

    /** First-frame latency compensation (ms). Mirror of face receiver's value;
     *  same purpose: subtracted from the captured audio clock when anchoring
     *  MessageAudioOffset for each new utterance. Tune empirically while
     *  watching PIE — small changes (50 ms) are perceptible. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Estuary|BodyAnimation",
              meta=(DisplayName="First Frame Latency Compensation (ms)",
                    ClampMin="0.0", ClampMax="3000.0",
                    UIMin="0.0",    UIMax="1500.0",
                    ToolTip="Compensates server-side latency at utterance start. Default 150 ms; tune empirically if drift visible."))
    float FirstFrameLatencyCompensationMs = 150.0f;

    // -----------------------------------------------------------------------
    // Diagnostic read-outs (BlueprintPure for stats UI; mirror of face receiver).
    // -----------------------------------------------------------------------

    /** Number of ticks where audio-animation drift exceeded kDriftThresholdMs. */
    UFUNCTION(BlueprintPure, Category="Estuary|BodyAnimation",
              meta=(DisplayName="Get Drift Exceeded Count",
                    ToolTip="Number of ticks where audio-animation drift exceeded the 100 ms threshold."))
    int32 GetDriftExceededCount() const { return DriftExceededCount; }

    /** Most-recent measured drift in milliseconds (+ahead / -behind). */
    UFUNCTION(BlueprintPure, Category="Estuary|BodyAnimation",
              meta=(DisplayName="Get Last Drift Ms",
                    ToolTip="Most recent audio-animation drift in milliseconds."))
    float GetLastDriftMs() const { return LastDriftMs; }

    // -----------------------------------------------------------------------
    // UFUNCTION delegate receivers (Pitfall 8 — UFUNCTION required for AddDynamic).
    // -----------------------------------------------------------------------

    /** Bound to UEstuaryCharacterComponent::OnBotPose in BeginPlay.
     *  Resets per-utterance state on MessageId change; pushes Pose to Buffer. */
    UFUNCTION()
    void HandleBotPoseEvent(const FEstuaryBotPose& Pose);

    /** Bound to UEstuaryCharacterComponent::OnInterruptAcked in BeginPlay.
     *  Clears Buffer (drops the interrupted utterance's in-flight frames) and
     *  arms the resume blend. The tick's HOLD path then re-publishes the last
     *  pose until the idle pump's stream arrives, and the active path slerps
     *  from the held pose into it over kResumeBlendSecs. */
    UFUNCTION()
    void HandleInterruptAckedEvent(const FEstuaryInterruptPayload& Payload);

private:
    // -----------------------------------------------------------------------
    // Constants
    // -----------------------------------------------------------------------

    /** Slerp window from the held pose into the resuming stream after an
     *  interrupt or HOLD gap. Long enough to conceal the transport seam,
     *  short enough not to lag the live motion perceptibly. */
    static constexpr float kResumeBlendSecs = 0.2f;

    /** Drift warn threshold (mirror face: 100 ms; logged at 1 Hz when exceeded). */
    static constexpr float kDriftThresholdMs = 100.0f;

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    /** Resolve subject name: <characterId>_Body_GestureSync.
     *  Resolution: WeakCharComp->GetLiveLinkSubjectName() + "_Body_GestureSync" suffix; falls
     *  back to "default_Body_GestureSync" if WeakCharComp is invalid. Single-place enforcement
     *  of the body subject naming convention. */
    FName ResolveSubjectName() const;

    // -----------------------------------------------------------------------
    // Internal state (mirror of face receiver's private members)
    // -----------------------------------------------------------------------

    /** Cached sibling component pointers (Pitfall 3 — TWeakObjectPtr cross-component). */
    TWeakObjectPtr<UEstuaryCharacterComponent>   WeakCharComp;
    TWeakObjectPtr<UEstuaryAudioPlayerComponent> WeakAudioComp;

    /** Body-pose frame buffer — TEstuaryAnimationFrameBuffer<FEstuaryBotPose>
     *  shipped by Task 1 of this plan. Sorted ring buffer + audio-clock-anchored
     *  per-bone Slerp interpolation. */
    TEstuaryAnimationFrameBuffer<FEstuaryBotPose> Buffer;

    /** Local kill-switch for the body receiver's publish path (default on). The
     *  server is the authoritative gate: it only emits bot_pose when
     *  session_info.animation_capabilities.body.active is true (deserialized into
     *  FEstuarySessionInfo::AnimationCapabilities and surfaced as a Warning by
     *  UEstuarySubsystem::HandleSessionInfo when the client opted in but the gate is
     *  closed). Because emission is server-gated, the receiver can stay ready
     *  unconditionally; flip this to false only to hard-disable publishing locally. */
    bool  bAnimationEnabled    = true;

    /** Resume blend state (2026-06-10, replaces the interrupt ramp-to-bind-pose).
     *
     *  bResumeBlendPending is armed by HandleInterruptAckedEvent, by new-utterance
     *  message handover (HandleBotPoseEvent — conceals the ~kTargetLagSec playhead
     *  discontinuity across Buffer.Clear()), and by HOLD
     *  ticks (empty buffer). The active publish path consumes it: snapshots the
     *  held pose into ResumeBlendSourceTransforms and slerps from that FIXED
     *  snapshot toward the live stream pose with a linear alpha over
     *  kResumeBlendSecs (snapshot-as-source prevents per-tick compounding —
     *  same rationale as the earlier ramp refactor). */
    bool  bResumeBlendPending        = false;
    bool  bResumeBlending            = false;
    float ResumeBlendTimeAccumulated = 0.0f;

    /** Pose held at the seam (snapshot of LastPublishedTransforms when the
     *  stream resumed). Source pose for the resume blend; cleared when the
     *  blend completes or the bone schema changes mid-blend (snap instead). */
    TArray<FTransform> ResumeBlendSourceTransforms;

    /** Message id named by the last `interrupt` event. In-flight frames of the
     *  interrupted utterance that land AFTER the ack (cross-pod routing can
     *  reorder vs the gateway's drop-map update) are dropped in
     *  HandleBotPoseEvent so they can't re-fill the just-cleared buffer and
     *  resurrect the dead utterance. Cleared when a different message id
     *  arrives (contract: "clear their own pose buffer ... defend against
     *  frames already in flight"). */
    FString InterruptedMessageId;

    /** Drift telemetry (mirror face). */
    int32  DriftExceededCount     = 0;
    float  LastDriftMs            = 0.0f;
    double LastDriftWarnAtSeconds = 0.0;

    /** Per-utterance audio-clock offset.
     *  Same rationale as face: GetPlaybackTimeSeconds() is cumulative across the
     *  audio player's lifetime, but bot_pose TimeCodeSec restarts at ~0 each
     *  utterance. We capture the audio clock value when each new MessageId's first
     *  frame arrives, then subtract it to recover a per-utterance time base.
     *  2026-06-10: additionally trimmed each tick by the adaptive-playout slew
     *  (EstuaryPlayheadMath.h) so the playhead keeps ~kTargetLagSec of buffered
     *  frames — speech supply is only ~1x real-time, and a fixed anchor starved
     *  into clamp-on-latest (freeze-frames while speaking). */
    FString CurrentMessageId;
    float   MessageAudioOffset = 0.0f;

    /** Wall-clock fallback for idle messages (worker silence-fill regime).
     *  GetPlaybackTimeSeconds() freezes between bot speech because BytesConsumed
     *  stops advancing once the audio thread drains the queue. Idle frames stream
     *  at 30 fps from GestureSync and would all map to the same RelativeAudioTime,
     *  rendering as a single frozen pose. For idle-stream MessageIds (the
     *  `__sarah_idle__` PREFIX — ids are unique per idle resume, e.g.
     *  `__sarah_idle__#7` — or a legacy `idle:` prefix) we anchor
     *  MessageWallClockOffset = FApp::GetCurrentTime() - latency-comp
     *  and derive RelativeAudioTime from the wall clock, so idle motion plays
     *  smoothly. Real (TTS-driven) utterances keep the audio-clock anchor as the
     *  free-run time-base (playback pauses with the audio), with the same
     *  adaptive-playout slew maintaining the buffered lead (2026-06-10). */
    bool   bUseWallClockForCurrentMessage = false;
    double MessageWallClockOffset         = 0.0;

    // --- Lag diagnostics: counters for the throttled [BodyDiag] line in
    //     TickComponent (frame supply rate, re-anchors, HOLD/underrun ticks). ---
    int32  DiagFramesRecvWindow = 0;   // non-terminator bot_pose frames since last [BodyDiag]
    int32  DiagReanchorWindow   = 0;   // catastrophic playhead re-anchors (either clock mode) since last [BodyDiag]
    int32  DiagHoldTicksWindow  = 0;   // bind-pose HOLD (underrun) ticks since last [BodyDiag]
    double DiagLastLogSeconds   = 0.0; // throttle anchor for the [BodyDiag] line

    /** Rig gate latch (SDK_CONTRACT §session_info.animation_capabilities.body.rig).
     *  False until session_info has landed (SessionId non-empty) and body.rig has
     *  been checked against "mhr63". On mismatch the component tick is disabled
     *  permanently for the play session with ONE Error log — no silent frame
     *  drops between mixed client/server versions (spec §8). */
    bool bRigGateLatched = false;

    /** 1 Hz throttle anchor for the bone-count Error in HandleBotPoseEvent
     *  (non-terminator frames with Bones.Num() != EstuaryMHR::kNumBones are
     *  dropped with a LOUD Error, not Verbose — spec §8). */
    double LastBoneCountErrorLogSeconds = 0.0;

    /** True only on the first frame of a new utterance; receiver calls
     *  Pub->EnsureSubject(SubjectName, BoneNames, BoneParents) to (re)build the
     *  per-subject skeleton schema. */
    bool bSchemaEnsuredForCurrentMessage = false;

    /** Pre-allocated per-tick scratchpad to avoid per-tick allocation (Pitfall 6 —
     *  Reserve(EstuaryMHR::kNumBones=63) at construction). Reused across HOLD +
     *  active-publish paths. Index 0 = root with translation; rest pure rotation. */
    TArray<FTransform> LastPublishedTransforms;

    /** Cached schema (passed to EnsureSubject on first publish of each new
     *  utterance). 63-entry MHR wire-name array (wire name == skeleton bone name
     *  on the generated MHR FBX) + 63-entry parent-index array from the generated
     *  EstuaryMHR::kBoneParents table. */
    TArray<FName> CachedBoneNames;
    TArray<int32> CachedBoneParents;
};
