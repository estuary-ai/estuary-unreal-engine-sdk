// EstuaryBodyBoneRemapLibrary.cpp — UEstuaryBodyBoneRemapLibrary implementation.
// Pitfall 6:  Missing-row warning emitted once per unique bone name (WarnedBones TSet).
// Pitfall 23: IWYU only, no CoreMinimal.h.
//
// Mirror of EstuaryArkitRemapLibrary.cpp with field-type swaps:
//   FString→FName for bone names; ScaleFactor→RollOffsetDeg.
//   TMap<FString, float> ApplyRemapToWeights → TArray<FTransform> ApplyRemapToFrame.
// ApplyRemapToFrame additionally calls EstuaryBodyFrameTransform::ToUEQuatFromGestureSync
// for the per-bone quat conversion + ToUEHipsFromGestureSync for index 0's location.

#include "EstuaryBodyBoneRemap.h"

#include "EstuaryBodyFrameTransform.h"
#include "EstuaryLog.h"
#include "EstuaryTypes.h"
#include "Engine/DataTable.h"
#include "Math/Transform.h"
#include "Math/Quat.h"
#include "Math/Vector.h"
#include "Math/UnrealMathUtility.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"

bool UEstuaryBodyBoneRemapLibrary::ApplyRemap(
    const FString& InputName,
    const UDataTable* Table,
    FName& UEBoneName,
    float& RollOffsetDeg)
{
    // Identity fallback — caller can use these values safely even on miss.
    UEBoneName    = FName(*InputName);
    RollOffsetDeg = 0.0f;

    if (!Table) return false;

    const FEstuaryBodyBoneRemapRow* Row =
        Table->FindRow<FEstuaryBodyBoneRemapRow>(FName(*InputName),
            TEXT("UEstuaryBodyBoneRemapLibrary::ApplyRemap"),
            /*bWarnIfRowMissing=*/false);  // suppress DataTable's own warning; we handle it

    if (!Row)
    {
        // One-time warning per unknown bone name to avoid log spam (Pitfall 6).
        // WarnedBones is a process-lifetime static — reset on restart is correct.
        // Independent from face's WarnedCurves so the two warning sets do not
        // interfere across modalities.
        static TSet<FString> WarnedBones;
        bool bAlreadyInSet = false;
        WarnedBones.Add(InputName, &bAlreadyInSet);
        if (!bAlreadyInSet)
        {
            UE_LOG(LogEstuary, Warning,
                TEXT("[BodyBoneRemap] Bone '%s' not found in remap table. "
                     "Falling back to identity name + 0 roll. "
                     "Check your DT_GestureSyncBoneRemap_Identity DataTable. "
                     "(further occurrences silenced)"),
                *InputName);
        }
        return false;
    }

    UEBoneName    = Row->UEBoneName;
    RollOffsetDeg = Row->RollOffsetDeg;
    return true;
}

void UEstuaryBodyBoneRemapLibrary::ApplyRemapToFrame(
    const UDataTable* Table,
    const FEstuaryBotPose& Frame,
    TArray<FTransform>& OutTransforms,
    TArray<FName>& OutBoneNames)
{
    OutTransforms.Reset();
    OutBoneNames.Reset();

    const int32 BoneCount = Frame.Bones.Num();
    OutTransforms.Reserve(BoneCount);
    OutBoneNames.Reserve(BoneCount);

    // Defensive: iterate Bones.Num() rather than assuming 52 —
    // length-flex survival guarantee even though wire is fixed at J=52.
    for (int32 i = 0; i < BoneCount; ++i)
    {
        const FEstuaryBotPoseBone& InBone = Frame.Bones[i];

        FName  UEBoneName;
        float  RollOffsetDeg = 0.0f;
        ApplyRemap(InBone.Name, Table, UEBoneName, RollOffsetDeg);

        // FEstuaryBotPoseBone::Quat is FVector4 carrying [x, y, z, w] (per
        // EstuaryTypes.h FEstuaryBotPoseBone documentation).
        const FQuat InQuat(InBone.Quat.X, InBone.Quat.Y, InBone.Quat.Z, InBone.Quat.W);
        const FQuat OutQuat = EstuaryBodyFrameTransform::ToUEQuatFromGestureSync(InQuat, RollOffsetDeg);

        FTransform Xform;
        Xform.SetRotation(OutQuat);

        // Hips (index 0) carries world-space translation from the wire.
        // Convert SMPL-X meters → UE cm + axis swap and write to Xform.Location.
        // Other bones keep zero translation (local rotation-only — translation
        // comes from the rest pose at the rig level).
        if (i == 0)
        {
            Xform.SetLocation(EstuaryBodyFrameTransform::ToUEHipsFromGestureSync(Frame.HipsWorld));
        }

        OutTransforms.Add(Xform);
        OutBoneNames.Add(UEBoneName);
    }
}
