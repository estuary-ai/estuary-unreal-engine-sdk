// EstuaryFaceAnimInstance.h — DEPRECATED STUB.
//
// The 52 ARKit-52 UPROPERTY / Modify Curve pipeline this class previously hosted has
// been replaced by the Live Link-driven flow in EstuaryLiveLink (Path B —
// Live Link publishing). The class is retained for ONE
// minor version so user blueprints that reparented Face_AnimBP to
// UEstuaryFaceAnimInstance under the old guide still load (UCLASS name / reflected
// class path are KEPT IDENTICAL — see rationale below). Migrate by reparenting back
// to stock UAnimInstance and dropping a LiveLinkPose AnimGraph node configured with
// the character's LiveLinkSubjectName.
//
// Scheduled for deletion in v2.
//
// Pitfall 23: #pragma once, IWYU, no CoreMinimal.h.

#pragma once

#include "CoreTypes.h"
#include "UObject/ObjectMacros.h"
#include "Animation/AnimInstance.h"

#include "EstuaryFaceAnimInstance.generated.h"

/**
 * UEstuaryFaceAnimInstance (DEPRECATED — see comment header).
 *
 * Empty stub retained for v1.x migration safety. Do not use for new content.
 * Use the LiveLinkPose AnimGraph node + EstuaryLiveLink module instead.
 *
 * Why no `UCLASS(Deprecated)`: UE 5.7 UHT rejects `UCLASS(Deprecated)` unless the
 * C++ class identifier is renamed to `UDEPRECATED_EstuaryFaceAnimInstance`. That
 * rename is reflected in the .uasset parent-class path, so any user `Face_AnimBP`
 * reparented under the old guide would fail to load with "missing parent class".
 * Migration safety wins over editor-side deprecation warnings; class name is
 * preserved verbatim and `NotBlueprintable` discourages new-content authoring.
 * Deprecation is surfaced via README, METAHUMAN_ABP_GUIDE.md, and this header.
 *
 * Scheduled for hard deletion in v2 once downstream content has migrated.
 */
UCLASS(NotBlueprintable,
       meta = (DisplayName = "Estuary Face AnimInstance (DEPRECATED)",
               ToolTip = "DEPRECATED. Replaced by Live Link in EstuaryLiveLink. Do not use for new content."))
class ESTUARY_API UEstuaryFaceAnimInstance : public UAnimInstance
{
    GENERATED_BODY()
    // No body — this stub exists solely so reparented user blueprints continue to load.
};
