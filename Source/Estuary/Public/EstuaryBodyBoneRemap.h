// EstuaryBodyBoneRemap.h — DataTable-driven SMPL-X→MetaHuman bone-name remap (UE-04).
// Pitfall 23: #pragma once, IWYU only, no CoreMinimal.h.
//
// Mirror of EstuaryArkitRemap.h: USTRUCT FTableRowBase row + UBlueprintFunctionLibrary
// helpers. Drops the EFaceRigProfile enum (face-only concept; body uses IKRetargeter
// asset selection for per-rig variation, not a per-rig profile enum on the data side).
//
// Rows are intentionally minimal — RowName (= SMPL-X canonical bone name),
// UEBoneName (target MetaHuman FName), RollOffsetDeg (optional per-bone twist).
// The 4 SMPL-X→UE coordinate conversion failure modes (axis swap, sign flip,
// handedness, roll) are NOT spread across 52 rows — they live in a SINGLE math
// helper EstuaryBodyFrameTransform::ToUEQuatFromGestureSync. The DataTable carries
// per-bone empirical roll offsets only when the 6-pose test diagnoses Roll mode
// for that specific bone.

#pragma once

#include "CoreTypes.h"
#include "UObject/ObjectMacros.h"
#include "UObject/NameTypes.h"
#include "Engine/DataTable.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Containers/Array.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "Math/Transform.h"

#include "EstuaryBodyBoneRemap.generated.h"

// Forward decls — keep IWYU footprint minimal in the header.
struct FEstuaryBotPose;       // defined in EstuaryTypes.h
class UDataTable;

// ---------------------------------------------------------------------------
// FEstuaryBodyBoneRemapRow — one row of DT_GestureSyncBoneRemap_Identity.csv.
// ---------------------------------------------------------------------------

/**
 * Single row in the GestureSync (SMPL-X) → MetaHuman bone-name remap DataTable.
 *
 * RowName (the DataTable's implicit key) = SMPL-X canonical bone name (e.g.
 * "hips", "spine", "leftIndex1"). UEBoneName = MetaHuman FName the wire bone
 * remaps to (e.g. "pelvis", "spine_01", "index_01_l"). RollOffsetDeg = optional
 * per-bone roll about UE local +X (default 0; populate empirically when the
 * 6-pose test diagnoses Roll mode for this bone).
 *
 * Minimal row — name + per-bone optional roll. Global axis/sign/
 * handedness conversion lives in EstuaryBodyFrameTransform::ToUEQuatFromGestureSync,
 * NOT spread across 52 rows.
 */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryBodyBoneRemapRow : public FTableRowBase
{
    GENERATED_BODY()

    /** Target MetaHuman bone name. FName because Live Link skeletons use FName. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Estuary",
              meta=(ToolTip="Target MetaHuman bone name. FName because Live Link skeletons use FName."))
    FName UEBoneName;

    /** Optional per-bone roll offset (degrees) applied about UE local +X
     *  (bone-forward axis convention). Default 0; populate empirically when
     *  6-pose test diagnoses Roll mode for this bone. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Estuary",
              meta=(ToolTip="Optional per-bone roll offset (degrees). Default 0; populate empirically when 6-pose test diagnoses Roll mode for this bone."))
    float RollOffsetDeg = 0.0f;
};

// ---------------------------------------------------------------------------
// UEstuaryBodyBoneRemapLibrary — DataTable lookup + frame-conversion helpers.
// ---------------------------------------------------------------------------

/**
 * Blueprint function library for GestureSync (SMPL-X) → MetaHuman bone-name remap +
 * coordinate conversion. Mirror of UEstuaryArkitRemapLibrary with the role
 * swap: FString→FName for bone names, ScaleFactor→RollOffsetDeg for the
 * per-row scalar, TMap<FString,float>→TArray<FTransform> for the per-frame
 * batch helper.
 *
 * One-time-warning-per-unknown-bone (Pitfall 6 mirror): unknown bone names
 * are logged once each via a static TSet, then silenced.
 */
UCLASS()
class ESTUARY_API UEstuaryBodyBoneRemapLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /**
     * Look up a single SMPL-X canonical bone name in the remap table.
     *
     * Defaults are populated BEFORE the early return so callers always get a
     * sane fallback even on miss:
     *   UEBoneName    = FName(*InputName)   // identity passthrough
     *   RollOffsetDeg = 0.0f
     *
     * On miss, emits a one-time LogEstuary Warning per unique missing bone
     * name (Pitfall 6 mirror).
     *
     * @param InputName       SMPL-X canonical bone name (e.g. "hips").
     * @param Table           DataTable with FEstuaryBodyBoneRemapRow rows.
     * @param UEBoneName      [out] MetaHuman bone FName (or identity FName on miss).
     * @param RollOffsetDeg   [out] Per-bone roll offset deg (or 0.0f on miss).
     * @return                True if the row was found.
     */
    UFUNCTION(BlueprintCallable, BlueprintPure=false, Category="Estuary|BodyAnimation",
              meta=(DisplayName="Apply Body Bone Remap",
                    ToolTip="Look up a SMPL-X canonical bone name in the DataTable; returns the MetaHuman FName + per-bone roll offset (deg). One-time warning per unknown bone name."))
    static bool ApplyRemap(
        const FString& InputName,
        const UDataTable* Table,
        FName& UEBoneName,
        float& RollOffsetDeg);

    /**
     * Apply remap + ToUEQuatFromGestureSync(quat, RollOffsetDeg) conversion to all bones
     * in a frame. For Bones[0] (hips), also converts the frame's HipsWorld via
     * ToUEHipsFromGestureSync(meters→cm) and writes it as the FTransform's location.
     *
     * Caller-supplied OutTransforms / OutBoneNames are reset and refilled — caller
     * may pre-allocate via Reserve(Frame.Bones.Num()) at the call site to avoid
     * per-tick reallocation (Pitfall 6 — no per-tick allocation here).
     *
     * @param Table           Remap DataTable. If null, identity-name passthrough
     *                        with the global ToUEQuatFromGestureSync conversion still applies.
     * @param Frame           Source GestureSync bot_pose frame (52 bones expected; iterates
     *                        Bones.Num() defensively for length-flex safety).
     * @param OutTransforms   [out] Per-bone FTransform array (rotation always set;
     *                        location set on index 0 only).
     * @param OutBoneNames    [out] Per-bone MetaHuman FName array (parallel to OutTransforms).
     */
    static void ApplyRemapToFrame(
        const UDataTable* Table,
        const FEstuaryBotPose& Frame,
        TArray<FTransform>& OutTransforms,
        TArray<FName>& OutBoneNames);
};
