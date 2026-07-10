// EstuaryBodyLiveLinkPublisher.cpp — Live Link source for GestureSync bot_pose body animation (UE-02).
//
// Mirror of FEstuaryLiveLinkPublisher (face) with role swap:
//   - Static data: FLiveLinkSkeletonStaticData with FULL BoneNames + BoneParents
//     (52 each), pushed via ULiveLinkAnimationRole::StaticClass(). Body has NO curves
//     (PropertyNames left empty). This is the OPPOSITE of face's empty-bones hack —
//     body is the actual intended use of ULiveLinkAnimationRole (Pitfall 4 mitigation:
//     "ULiveLinkAnimationRole (transforms), not ULiveLinkBasicRole (curves)").
//   - Frame data: FLiveLinkAnimationFrameData with Transforms (52 entries; index 0 =
//     hips with translation, rest pure rotation). PropertyValues left empty.
//
// The receiver is responsible for:
//   - Pre-converting SMPL-X→UE quaternions via FEstuaryBodyFrameTransform::ToUEQuatFromGestureSync
//     (UE-05; locked formula q_ue = (-qz, -qx, -qy, qw))
//   - Pre-allocating Transforms TArray to avoid per-tick allocation
// The publisher only sees POST-CONVERSION + POST-REMAP MetaHuman names + UE transforms.
//
// Pitfall 7: lives ONLY in EstuaryLiveLink module; the IEstuaryBodyLiveLinkPublisher
// interface lives in core. Core has ZERO LiveLink/* includes.
// Pitfall 23: IWYU only, no CoreMinimal.h.
//
// Phase 4 — UE-02.

#include "EstuaryBodyLiveLinkPublisher.h"
#include "EstuaryLog.h"

#include "ILiveLinkClient.h"
#include "LiveLinkTypes.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "Math/UnrealMathUtility.h"

// ============================================================================
// ILiveLinkSource overrides — mirror face publisher verbatim (only debug log
// prefix differs: "[EstuaryBodyLiveLinkPublisher]" vs. "[EstuaryLiveLinkPublisher]").
// ============================================================================

void FEstuaryBodyLiveLinkPublisher::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
    Client     = InClient;
    SourceGuid = InSourceGuid;
    UE_LOG(LogEstuary, Log,
        TEXT("[EstuaryBodyLiveLinkPublisher] ReceiveClient — guid=%s"),
        *SourceGuid.ToString());
}

bool FEstuaryBodyLiveLinkPublisher::RequestSourceShutdown()
{
    Client = nullptr;
    return true;
}

// ============================================================================
// Push static data for a subject — body's adaptation of face's pattern.
//
// CRITICAL DEVIATION FROM FACE (Pitfall 4 mitigation):
//   Face uses ULiveLinkAnimationRole with EMPTY BoneNames + BoneParents (a hack
//   to satisfy LiveLinkPose's "skeleton role required" rejection of BasicRole;
//   face publishes only PropertyNames+PropertyValues for its 52 ARKit curves).
//   Body uses ULiveLinkAnimationRole with FULL BoneNames + BoneParents + (in
//   PublishFrame) Transforms — the actual intended use. Body has NO curves
//   (PropertyNames left empty).
// ============================================================================

void FEstuaryBodyLiveLinkPublisher::PushStaticDataForSubject(
    const FName& SubjectName, FBodySubjectState& State)
{
    if (!Client) { return; }
    if (State.BoneNames.Num() == 0) { return; }
    if (State.BoneParents.Num() != State.BoneNames.Num())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[EstuaryBodyLiveLinkPublisher] BoneParents count (%d) != BoneNames count (%d) "
                 "for subject '%s' — skipping static data push."),
            State.BoneParents.Num(), State.BoneNames.Num(), *SubjectName.ToString());
        return;
    }

    FLiveLinkStaticDataStruct StaticDataStruct(FLiveLinkSkeletonStaticData::StaticStruct());
    FLiveLinkSkeletonStaticData* SkelData =
        StaticDataStruct.Cast<FLiveLinkSkeletonStaticData>();
    if (!SkelData) { return; }

    // FULL skeleton (vs. face's empty-bones hack — Pitfall 4 mitigation).
    SkelData->BoneNames   = State.BoneNames;     // 52 FNames in MetaHuman skeleton order (post-remap)
    SkelData->BoneParents = State.BoneParents;   // 52 int32 parents (rebased to MetaHuman skeleton order)
    SkelData->PropertyNames.Reset();             // body has no curves

    // ULiveLinkAnimationRole (transforms), NOT ULiveLinkBasicRole (curves) —
    // the body success criterion.
    Client->PushSubjectStaticData_AnyThread(
        FLiveLinkSubjectKey{ SourceGuid, SubjectName },
        ULiveLinkAnimationRole::StaticClass(),
        MoveTemp(StaticDataStruct));

    UE_LOG(LogEstuary, Log,
        TEXT("[EstuaryBodyLiveLinkPublisher] Pushed static data for subject '%s' "
             "(%d bones, role=AnimationRole)."),
        *SubjectName.ToString(), State.BoneNames.Num());
}

// ============================================================================
// IEstuaryBodyLiveLinkPublisher::EnsureSubject
//
// Idempotent — safe to call every tick. Schema (BoneNames + BoneParents) is
// pushed exactly once per (subject, schema) pair. A schema change (different
// BoneNames array) forces a re-push (rare; typically only if the receiver
// rebuilds against a different DataTable mid-session, which is not expected
// in v2.0 — but the path is defensive).
// ============================================================================

void FEstuaryBodyLiveLinkPublisher::EnsureSubject(
    const FName& SubjectName,
    const TArray<FName>& BoneNames,
    const TArray<int32>& BoneParents)
{
    if (!Client) { return; }

    FScopeLock Lock(&SubjectsLock);
    FBodySubjectState& State = Subjects.FindOrAdd(SubjectName);

    // Schema-change detection: if caller passed non-empty BoneNames AND they differ
    // from the cached BoneNames, treat this as a schema change and force re-push.
    // Defensive nullptr/empty probe (caller's defensive idempotent self-call from
    // PublishFrame) does NOT clobber an established schema.
    if (BoneNames.Num() > 0 && BoneNames != State.BoneNames)
    {
        if (State.bPushedStaticData)
        {
            UE_LOG(LogEstuary, Warning,
                TEXT("[EstuaryBodyLiveLinkPublisher] BoneNames changed for subject '%s' "
                     "(was %d bones, now %d) — re-pushing static data."),
                *SubjectName.ToString(),
                State.BoneNames.Num(), BoneNames.Num());
        }
        State.BoneNames         = BoneNames;
        State.BoneParents       = BoneParents;
        State.bPushedStaticData = false;
    }

    // Push static data exactly once per (subject, schema) pair.
    if (!State.bPushedStaticData && State.BoneNames.Num() > 0)
    {
        PushStaticDataForSubject(SubjectName, State);
        State.bPushedStaticData = true;
    }
}

// ============================================================================
// IEstuaryBodyLiveLinkPublisher::PublishFrame
//
// Pushes one frame's 52 transforms (index 0 = hips with translation; rest pure
// rotation). Mirrors face's PublishFrame structure verbatim except for the data
// shape (Transforms vs. PropertyValues). Includes the same monotonic WorldTime
// guard (face lines 252-265) — Live Link rejects duplicate-time frames with
// "subject is invalid" warning.
// ============================================================================

void FEstuaryBodyLiveLinkPublisher::PublishFrame(
    const FName& SubjectName,
    const TArray<FTransform>& Transforms,
    double WorldTime,
    const FString& MessageId)
{
    if (!Client) { return; }

    // Defensive idempotent self-call (face line 217 mirror). No-ops if state is
    // already established with non-empty BoneNames; protects against a
    // publish-without-prior-ensure path (e.g., during plugin reload).
    EnsureSubject(SubjectName, /*BoneNames=*/{}, /*BoneParents=*/{});

    FScopeLock Lock(&SubjectsLock);
    FBodySubjectState* State = Subjects.Find(SubjectName);
    if (!State || !State->bPushedStaticData)
    {
        // Still couldn't establish a schema — bail without pushing a misordered frame.
        return;
    }

    if (Transforms.Num() != State->BoneNames.Num())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[EstuaryBodyLiveLinkPublisher] PublishFrame: Transforms count (%d) != "
                 "BoneNames count (%d) for subject '%s' — skipping."),
            Transforms.Num(), State->BoneNames.Num(), *SubjectName.ToString());
        return;
    }

    FLiveLinkFrameDataStruct FrameDataStruct(FLiveLinkAnimationFrameData::StaticStruct());
    FLiveLinkAnimationFrameData* FrameData =
        FrameDataStruct.Cast<FLiveLinkAnimationFrameData>();
    if (!FrameData) { return; }

    // Body publishes 52 transforms — index 0 = hips with translation, rest pure rotation.
    // Caller (receiver) is responsible for pre-allocating + pre-converting (UE-05).
    // Use Reset+Append to avoid an extra copy vs. operator=.
    FrameData->Transforms.Reset(Transforms.Num());
    FrameData->Transforms.Append(Transforms);
    FrameData->PropertyValues.Reset();   // body has no curves

    // Monotonic WorldTime guard (face lines 252-265 mirror). Live Link rejects frames
    // whose WorldTime equals the previous frame's WorldTime ("subject is invalid" with
    // "(01) A new frame data ... has the same time as a previous frame" warning).
    // Bump by 1 microsecond past the last published value if needed.
    double EffectiveWorldTime = WorldTime;
    if (EffectiveWorldTime <= State->LastPublishedWorldTime)
    {
        EffectiveWorldTime = State->LastPublishedWorldTime + 1e-6;
    }
    State->LastPublishedWorldTime = EffectiveWorldTime;
    FrameData->WorldTime = FLiveLinkWorldTime(EffectiveWorldTime);

    // Diagnostic stamping — LiveLinkPose ignores MetaData; this is for recorders /
    // debug UI / future server-side interrupt audit. Same MetaData key as face for
    // cross-modality correlation.
    if (!MessageId.IsEmpty())
    {
        FrameData->MetaData.StringMetaData.Add(TEXT("estuary_message_id"), MessageId);
    }

    Client->PushSubjectFrameData_AnyThread(
        FLiveLinkSubjectKey{ SourceGuid, SubjectName },
        MoveTemp(FrameDataStruct));
}
