// EstuaryArkitRemap.h — ARKit-52 curve name remap types and library.
// Pitfall 6:  Remap table shipped from day one; missing-row warning prevents silent mismatch.
// Pitfall 23: #pragma once, IWYU, no CoreMinimal.h.

#pragma once

#include "CoreTypes.h"
#include "UObject/ObjectMacros.h"
#include "Engine/DataTable.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Containers/Map.h"
#include "Containers/Set.h"

#include "EstuaryArkitRemap.generated.h"

// ---------------------------------------------------------------------------
// EFaceRigProfile — which curve-name convention the target rig uses.
// ---------------------------------------------------------------------------

/**
 * Identifies the curve-name convention of the target MetaHuman face rig.
 *
 * Identity             — ARKit names passed through unchanged (scale=1.0).
 *                        Correct for MetaHuman_ARKit_Ready variants (post-5.2 MH).
 * MetaHuman_ARKit_Ready — Synonym for Identity; ARKit-ready rigs are already 1:1.
 *                         Separate entry to make user intent explicit in the editor.
 * MHA_v1               — MetaHuman Animator (MHA) curve convention. Names differ from
 *                         ARKit literals; requires a non-trivial remap table.
 *                         See PITFALLS.md and Epic's PA_MetaHuman_ARKit_Mapping.
 * Custom               — User-supplied DataTable overrides all mappings.
 */
UENUM(BlueprintType)
enum class EFaceRigProfile : uint8
{
    Identity               UMETA(DisplayName = "Identity (pass-through)"),
    MetaHuman_ARKit_Ready  UMETA(DisplayName = "MetaHuman ARKit-Ready"),
    MHA_v1                 UMETA(DisplayName = "MetaHuman Animator v1"),
    Custom                 UMETA(DisplayName = "Custom"),
};

// ---------------------------------------------------------------------------
// FEstuaryArkitRemapRow — one row in the DT_ArkitRemap DataTable.
// ---------------------------------------------------------------------------

/**
 * Single row in the ARKit remap DataTable.
 *
 * RowName (the DataTable's implicit key) = incoming ARKit wire curve name from backend.
 * TargetCurveName = curve name expected by the target MetaHuman rig.
 * ScaleFactor     = multiplier applied to the float weight (1.0 for identity).
 *
 * Editor import: Content Browser -> Import -> CSV -> Row Struct = FEstuaryArkitRemapRow.
 */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryArkitRemapRow : public FTableRowBase
{
    GENERATED_BODY()

    /**
     * The curve name the target rig expects.
     * For Identity profile: same as the RowName (ARKit wire name).
     * For MHA_v1: the corresponding MHA curve name.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Estuary")
    FString TargetCurveName;

    /**
     * Scale factor applied to the incoming weight before passing to ApplyWeights.
     * 1.0 for direct mapping; negative values can invert a curve (e.g., some MHA axes
     * are inverted relative to ARKit convention).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Estuary")
    float ScaleFactor = 1.0f;
};

// ---------------------------------------------------------------------------
// UEstuaryArkitRemapLibrary — static helpers for remap operations.
// ---------------------------------------------------------------------------

/**
 * Blueprint function library for ARKit remap operations.
 *
 * Primary use: UEstuaryAnimationReceiver calls ApplyRemapToWeights each tick
 * when a RemapAsset is configured. All methods are static; no instance required.
 */
UCLASS()
class ESTUARY_API UEstuaryArkitRemapLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:

    /**
     * Look up a single ARKit curve name in the remap table and return the
     * translated curve name and scale factor.
     *
     * If the row is not found:
     *   - Emits a one-time LogEstuary Warning per unique missing curve name
     *     (using a static TSet to suppress duplicate warnings — Pitfall 6).
     *   - Returns bFound=false; OutputName and Scale are set to identity fallback
     *     so callers can proceed without crashing.
     *
     * @param InputName    ARKit wire curve name from the backend payload.
     * @param Table        DataTable with FEstuaryArkitRemapRow rows.
     * @param OutputName   [out] Remapped curve name (or InputName on miss).
     * @param Scale        [out] Scale factor (or 1.0 on miss).
     * @return             True if the row was found.
     */
    UFUNCTION(BlueprintCallable, BlueprintPure=false, Category="Estuary|Animation",
              meta=(DisplayName="Apply ARKit Remap",
                    ToolTip="Look up InputName in Table and return the translated OutputName and Scale. Logs a one-time warning per unknown curve name."))
    static bool ApplyRemap(
        const FString& InputName,
        const UDataTable* Table,
        FString& OutputName,
        float& Scale);

    /**
     * Apply remap to an entire weights map in-place.
     * For each entry: looks up the key in Table, replaces key with OutputName,
     * multiplies value by Scale. Entries not in the table fall back to identity.
     * When two ARKit keys map to the same rig curve, the larger scaled value wins
     * (max-blend strategy).
     *
     * Called by UEstuaryAnimationReceiver::TickComponent when RemapAsset is set.
     *
     * @param Table    Remap DataTable. If null, Weights is returned unchanged.
     * @param Weights  [in/out] ARKit blendshape weights map to remap.
     */
    static void ApplyRemapToWeights(const UDataTable* Table, TMap<FString, float>& Weights);
};
