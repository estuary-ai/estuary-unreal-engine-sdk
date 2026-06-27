// EstuaryBodyFrameTransform.cpp — non-inline implementation of DiagnoseQuatResidual
// + EConversionFailureModeToString.
// Pitfall 23: IWYU only, no CoreMinimal.h.
//
// All other helpers (kHipsMetersToCm, ToUEQuatFromGestureSync, ToUEHipsFromGestureSync) are
// inline in the header — this .cpp exists solely for the diagnostic helper, which
// is large enough (~30 LOC) and only needed in test/error paths to warrant moving
// out of the header.

#include "EstuaryBodyFrameTransform.h"

namespace EstuaryBodyFrameTransform
{
    EConversionFailureMode DiagnoseQuatResidual(
        const FQuat& Expected,
        const FQuat& Observed,
        float Epsilon)
    {
        // Try each of 4 conversion-variant residuals. The first one whose error
        // against Observed is below Epsilon names the failure mode. If none
        // match, return None (residual is something else entirely — e.g. a
        // rotated bind-pose or a purely numerical drift).
        //
        // Residual variants assume the math helper produced Expected via the
        // canonical formula q_ue = (-qz, -qx, -qy, qw). Each variant simulates
        // a one-step deviation from the canonical formula:
        //   AxisSwap   — implementer forgot to negate vector components AND
        //                swapped Y↔Z without the full xyz remap (Y↔Z swap only)
        //   SignFlip   — implementer negated only X (not all of xyz)
        //   Handedness — implementer negated all of xyz (RH→LH inverse) but
        //                forgot the axis remap
        //   Roll       — implementer applied a 90° roll about +X by mistake
        //                (or omitted the proper RollOffsetDeg post-multiply)

        // 1. AxisSwap variant — Y↔Z swap without negation.
        const FQuat AxisSwapVariant(Expected.X, Expected.Z, Expected.Y, Expected.W);
        if (FQuat::ErrorAutoNormalize(AxisSwapVariant, Observed) < Epsilon)
        {
            return EConversionFailureMode::AxisSwap;
        }

        // 2. SignFlip variant — negate X only.
        const FQuat SignFlipVariant(-Expected.X, Expected.Y, Expected.Z, Expected.W);
        if (FQuat::ErrorAutoNormalize(SignFlipVariant, Observed) < Epsilon)
        {
            return EConversionFailureMode::SignFlip;
        }

        // 3. Handedness variant — negate all of xyz (RH→LH inverse without remap).
        const FQuat HandednessVariant(-Expected.X, -Expected.Y, -Expected.Z, Expected.W);
        if (FQuat::ErrorAutoNormalize(HandednessVariant, Observed) < Epsilon)
        {
            return EConversionFailureMode::Handedness;
        }

        // 4. Roll variant — Expected post-multiplied by an unintended +90° roll
        //    about UE local +X (the most common roll-mode mistake).
        const FQuat RollPost(FVector::ForwardVector, FMath::DegreesToRadians(90.0f));
        const FQuat RollVariant = Expected * RollPost;
        if (FQuat::ErrorAutoNormalize(RollVariant, Observed) < Epsilon)
        {
            return EConversionFailureMode::Roll;
        }

        return EConversionFailureMode::None;
    }

    const TCHAR* EConversionFailureModeToString(EConversionFailureMode Mode)
    {
        switch (Mode)
        {
            case EConversionFailureMode::None:       return TEXT("None");
            case EConversionFailureMode::AxisSwap:   return TEXT("AxisSwap");
            case EConversionFailureMode::SignFlip:   return TEXT("SignFlip");
            case EConversionFailureMode::Handedness: return TEXT("Handedness");
            case EConversionFailureMode::Roll:       return TEXT("Roll");
            default:                                 return TEXT("Unknown");
        }
    }
}
