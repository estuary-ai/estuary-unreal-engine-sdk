// EstuaryLiveLinkPublisher.cpp — Live Link source publisher implementation.
//
// Pushes ARKit-52 weights to ULiveLinkBasicRole subjects:
//   - Static data: 52 PropertyNames per subject (post-remap), pushed once per
//     (subject, schema) pair via PushSubjectStaticData_AnyThread.
//   - Frame data: 52 PropertyValues per tick (clamped 0..1, scaled per remap row),
//     plus MetaData["estuary_message_id"] + WorldTime, via
//     PushSubjectFrameData_AnyThread (internally locked — no marshal needed).
//
// ARKit→target-rig remap performed inside this class (Option A from RESEARCH).
// EnsureSubject builds PropertyNames from the canonical 52-name list lazily,
// applying ApplyRemap() row-by-row. PublishFrame uses SourceWireNames + ScaleFactors
// captured at EnsureSubject time to look up & scale incoming weights.
//
// Path B — Live Link publishing pipeline.
// Pitfall 23: IWYU, no CoreMinimal.h.

#include "EstuaryLiveLinkPublisher.h"
#include "EstuaryLog.h"
#include "EstuaryArkitRemap.h"

#include "ILiveLinkClient.h"
#include "LiveLinkTypes.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "Engine/DataTable.h"
#include "Math/UnrealMathUtility.h"

// ============================================================================
// Canonical 52 ARKit-52 names (mirrors EstuaryFaceAnimInstance.h ordering pre-Path-B,
// matching SDK_CONTRACT §animation_stream wire names exactly: 14 eye + 4 jaw +
// 24 mouth + 3 cheek + 2 nose + 5 brow + 1 tongue = 52). Wire names are the keys
// the receiver's interpolated TMap<FString,float> uses.
// ============================================================================

namespace
{
    const TArray<FString>& GetCanonicalArkit52Names()
    {
        // 14 eye + 4 jaw + 24 mouth + 3 cheek + 2 nose + 5 brow + 1 tongue = 52.
        // Order MUST match EstuaryFaceAnimInstance.h (legacy 52-UPROPERTY layout)
        // and SDK_CONTRACT §animation_stream wire names exactly.
        static const TArray<FString> Canonical = {
            // Eye (14)
            TEXT("eyeBlinkLeft"),       TEXT("eyeLookDownLeft"),    TEXT("eyeLookInLeft"),
            TEXT("eyeLookOutLeft"),     TEXT("eyeLookUpLeft"),      TEXT("eyeSquintLeft"),
            TEXT("eyeWideLeft"),        TEXT("eyeBlinkRight"),      TEXT("eyeLookDownRight"),
            TEXT("eyeLookInRight"),     TEXT("eyeLookOutRight"),    TEXT("eyeLookUpRight"),
            TEXT("eyeSquintRight"),     TEXT("eyeWideRight"),
            // Jaw (4)
            TEXT("jawForward"),         TEXT("jawLeft"),            TEXT("jawRight"),
            TEXT("jawOpen"),
            // Mouth (24)
            TEXT("mouthClose"),         TEXT("mouthFunnel"),        TEXT("mouthPucker"),
            TEXT("mouthLeft"),          TEXT("mouthRight"),         TEXT("mouthSmileLeft"),
            TEXT("mouthSmileRight"),    TEXT("mouthFrownLeft"),     TEXT("mouthFrownRight"),
            TEXT("mouthDimpleLeft"),    TEXT("mouthDimpleRight"),   TEXT("mouthStretchLeft"),
            TEXT("mouthStretchRight"),  TEXT("mouthRollLower"),     TEXT("mouthRollUpper"),
            TEXT("mouthShrugLower"),    TEXT("mouthShrugUpper"),    TEXT("mouthPressLeft"),
            TEXT("mouthPressRight"),    TEXT("mouthLowerDownLeft"), TEXT("mouthLowerDownRight"),
            TEXT("mouthUpperUpLeft"),   TEXT("mouthUpperUpRight"),
            // Cheek (3)
            TEXT("cheekPuff"),          TEXT("cheekSquintLeft"),    TEXT("cheekSquintRight"),
            // Nose (2)
            TEXT("noseSneerLeft"),      TEXT("noseSneerRight"),
            // Brow (5)
            TEXT("browDownLeft"),       TEXT("browDownRight"),      TEXT("browInnerUp"),
            TEXT("browOuterUpLeft"),    TEXT("browOuterUpRight"),
            // Tongue (1)
            TEXT("tongueOut"),
        };
        return Canonical;
    }
}

// ============================================================================
// ILiveLinkSource overrides
// ============================================================================

void FEstuaryLiveLinkPublisher::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
    Client     = InClient;
    SourceGuid = InSourceGuid;
    UE_LOG(LogEstuary, Log,
        TEXT("[EstuaryLiveLinkPublisher] ReceiveClient — guid=%s"),
        *SourceGuid.ToString());
}

bool FEstuaryLiveLinkPublisher::RequestSourceShutdown()
{
    Client = nullptr;
    return true;
}

// ============================================================================
// Schema construction — runs on game thread once per (subject, RemapAsset) pair.
// ============================================================================

void FEstuaryLiveLinkPublisher::BuildSubjectSchema(
    FSubjectState& OutState, const UDataTable* RemapAsset) const
{
    const TArray<FString>& Canonical = GetCanonicalArkit52Names();

    OutState.SourceWireNames.Reset(Canonical.Num());
    OutState.PropertyNames.Reset(Canonical.Num());
    OutState.ScaleFactors.Reset(Canonical.Num());

    for (const FString& WireName : Canonical)
    {
        OutState.SourceWireNames.Add(WireName);

        FString TargetName = WireName;
        float   Scale      = 1.0f;
        if (RemapAsset != nullptr)
        {
            // ApplyRemap returns true if the row is found; otherwise it logs once
            // (LogEstuary Warning per unique missing curve name) and leaves
            // OutputName / Scale at identity defaults.
            UEstuaryArkitRemapLibrary::ApplyRemap(WireName, RemapAsset, TargetName, Scale);
        }
        OutState.PropertyNames.Add(FName(*TargetName));
        OutState.ScaleFactors.Add(Scale);
    }

    OutState.RemapAsset        = RemapAsset;
    OutState.bPushedStaticData = false;
}

void FEstuaryLiveLinkPublisher::PushStaticDataForSubject(
    const FName& SubjectName, FSubjectState& State)
{
    if (!Client) { return; }

    // Use AnimationRole (skeleton + curves) so MetaHuman Face_AnimBP's stock
    // LiveLinkPose AnimGraph node accepts the subject. BasicRole would be cleaner
    // (curves only, no bones) but stock LiveLinkPose hard-rejects it as "subject is
    // invalid". AnimationRole's static data extends BaseStaticData, so PropertyNames
    // still carry our 52 ARKit-52 curve names; BoneNames/BoneParents stay empty
    // (we publish no skeleton).
    FLiveLinkStaticDataStruct StaticDataStruct(FLiveLinkSkeletonStaticData::StaticStruct());
    FLiveLinkSkeletonStaticData* SkelData =
        StaticDataStruct.Cast<FLiveLinkSkeletonStaticData>();
    if (!SkelData) { return; }

    SkelData->PropertyNames = State.PropertyNames;   // 52 FNames (inherited from base)
    SkelData->BoneNames.Reset();                      // no skeleton — curves only
    SkelData->BoneParents.Reset();

    Client->PushSubjectStaticData_AnyThread(
        FLiveLinkSubjectKey{ SourceGuid, SubjectName },
        ULiveLinkAnimationRole::StaticClass(),
        MoveTemp(StaticDataStruct));

    State.bPushedStaticData = true;

    UE_LOG(LogEstuary, Log,
        TEXT("[EstuaryLiveLinkPublisher] Pushed static data for subject '%s' "
             "(%d curve names, role=AnimationRole, RemapAsset=%s)."),
        *SubjectName.ToString(),
        State.PropertyNames.Num(),
        State.RemapAsset ? *State.RemapAsset->GetName() : TEXT("<identity>"));
}

// ============================================================================
// IEstuaryLiveLinkPublisher::EnsureSubject
// ============================================================================

void FEstuaryLiveLinkPublisher::EnsureSubject(
    const FName& SubjectName, const UDataTable* RemapAsset)
{
    if (!Client) { return; }

    FScopeLock Lock(&SubjectsLock);
    FSubjectState& State = Subjects.FindOrAdd(SubjectName);

    // Idempotency rule: if static data already pushed AND the caller is either
    // making a defensive nullptr probe OR confirming the existing remap, no-op.
    // (The receiver's first-of-utterance call passes its real RemapAsset; the
    // defensive PublishFrame fallback passes nullptr — that path must not clobber
    // an established schema, which would have caused per-tick re-push.)
    if (State.bPushedStaticData
        && (RemapAsset == nullptr || State.RemapAsset == RemapAsset))
    {
        return;
    }

    if (State.bPushedStaticData && State.RemapAsset != RemapAsset)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[EstuaryLiveLinkPublisher] RemapAsset pointer changed for subject '%s' "
                 "(was %s, now %s) — rebuilding schema + re-pushing static data."),
            *SubjectName.ToString(),
            State.RemapAsset ? *State.RemapAsset->GetName() : TEXT("<null>"),
            RemapAsset       ? *RemapAsset->GetName()       : TEXT("<null>"));
    }

    // (Re)build schema from RemapAsset, then push static data exactly once per pair.
    BuildSubjectSchema(State, RemapAsset);
    PushStaticDataForSubject(SubjectName, State);
}

// ============================================================================
// IEstuaryLiveLinkPublisher::PublishFrame
// ============================================================================

void FEstuaryLiveLinkPublisher::PublishFrame(
    const FName& SubjectName,
    const TMap<FString, float>& InterpolatedWeights,
    double WorldTime,
    const FString& MessageId)
{
    if (!Client) { return; }

    // Defensive EnsureSubject — no-ops if state already established. This protects
    // against a publish-without-prior-ensure path (e.g., during plugin reload) by
    // falling back to the identity schema when nothing else is registered.
    EnsureSubject(SubjectName, /*RemapAsset=*/nullptr);

    FScopeLock Lock(&SubjectsLock);
    FSubjectState* State = Subjects.Find(SubjectName);
    if (!State || !State->bPushedStaticData)
    {
        // Still couldn't establish a schema — bail without pushing a misordered frame.
        return;
    }

    // AnimationFrameData extends BaseFrameData (Transforms[] + inherited PropertyValues[]).
    // We publish empty Transforms (no skeleton) — only the curves matter.
    FLiveLinkFrameDataStruct FrameDataStruct(FLiveLinkAnimationFrameData::StaticStruct());
    FLiveLinkAnimationFrameData* FrameData =
        FrameDataStruct.Cast<FLiveLinkAnimationFrameData>();
    if (!FrameData) { return; }

    FrameData->Transforms.Reset();   // no skeleton — curves only

    // Build PropertyValues paired index-by-index with State->PropertyNames.
    FrameData->PropertyValues.Reserve(State->PropertyNames.Num());
    const int32 N = State->SourceWireNames.Num();
    for (int32 i = 0; i < N; ++i)
    {
        const FString& WireName = State->SourceWireNames[i];
        const float    Scale    = State->ScaleFactors[i];

        float Value = 0.0f;
        if (const float* W = InterpolatedWeights.Find(WireName))
        {
            Value = (*W) * Scale;
        }
        FrameData->PropertyValues.Add(FMath::Clamp(Value, 0.0f, 1.0f));
    }

    // Live Link rejects frames whose WorldTime equals the previous frame's
    // WorldTime ("subject is invalid" with "(01) A new frame data ... has the
    // same time as a previous frame" warning). FApp::GetCurrentTime() — what
    // the receiver typically passes — is stable within a single game frame;
    // any caller that publishes more than once per frame (multi-receiver
    // setups, ramp+settle path, etc.) will trip the dedup. Guarantee
    // monotonic strictly-increasing WorldTime per subject by bumping a
    // microsecond past the last published value if needed.
    double EffectiveWorldTime = WorldTime;
    if (EffectiveWorldTime <= State->LastPublishedWorldTime)
    {
        EffectiveWorldTime = State->LastPublishedWorldTime + 1e-6;
    }
    State->LastPublishedWorldTime = EffectiveWorldTime;

    FrameData->WorldTime = FLiveLinkWorldTime(EffectiveWorldTime);

    // Diagnostic stamping — LiveLinkPose ignores MetaData; this is for recorders /
    // debug UI / future server-side interrupt audit. Key chosen per RESEARCH §A11.
    if (!MessageId.IsEmpty())
    {
        FrameData->MetaData.StringMetaData.Add(TEXT("estuary_message_id"), MessageId);
    }

    Client->PushSubjectFrameData_AnyThread(
        FLiveLinkSubjectKey{ SourceGuid, SubjectName },
        MoveTemp(FrameDataStruct));
}
