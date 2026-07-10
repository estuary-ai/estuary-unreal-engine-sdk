// EstuaryBodyFrameTransform.h — pure-math MHR→UE coordinate conversion.
// Pitfall 23: #pragma once, IWYU only, no CoreMinimal.h, no Engine.h.
//
// Mirror of EstuaryAmplitudeMath.h pattern: pure-math, header-only namespace,
// namespaced inline helpers; no UObject deps. One non-inline function
// (DiagnoseQuatResidual) annotated with ESTUARY_API so its implementation can
// live in a tiny .cpp file.
//
// MHR wire frame (SDK_CONTRACT §body_animation_stream, MHR63):
//   - LOCAL joint rotations, xyzw, RH, +Y up, +Z forward; positions in meters.
//   - Identity != rest: at rest the wire locals equal MHR's A-pose rest local
//     frames (non-identity binds), NOT identity quats.
// UE frame: left-handed, +Z up, +X forward, centimeters.
//
// ============================== SOLVED ====================================
// SOLVED 2026-07-02 by scripts/ue_solve_mhr_axis.py against the imported
// SK_GestureSyncSource (48-perm FK ground-truth solve; quat winner err 0.0,
// runner-up 3.016; hips winner err 0.0, runner-up 16.0; world point map
// x,z,y @ scale 1.0 mean err 1e-05 cm; FK validation max 7.4e-05 cm over 8
// poses; PoseableMesh engine agreement ~1e-15 cm; see
// ue-mcp-artifacts/mhr_axis_solve.json). The provisional Phase 4 Y-mirror
// formulas did NOT survive the solve (confirms_provisional=false) — the MHR
// import maps to UE by a pure Y↔Z swap (RH Y-up → LH Z-up), so the quat
// axis part is swap-negated and hips scale m→cm.
// A uniform component map exists because a world-frame change acts on ALL
// bone locals by the same conjugation G·L·G⁻¹.
// Reimporting the FBX with different axis settings invalidates this —
// re-run the solver.
// ==========================================================================

#pragma once

#include "Math/UnrealMathUtility.h"
#include "Math/Quat.h"
#include "Math/Vector.h"

namespace EstuaryBodyFrameTransform
{
    /** MHR wire local quat (RH, +Y up, +Z forward, xyzw) → SK_GestureSyncSource
     *  bone-local quat.
     *
     *  SOLVED 2026-07-02 by scripts/ue_solve_mhr_axis.py (48-perm FK solve,
     *  winner "-x,-z,-y,w" err 0.0, runner-up 3.016 — see
     *  ue-mcp-artifacts/mhr_axis_solve.json). The MHR→UE frame change is a
     *  pure Y↔Z swap (RH Y-up → LH Z-up); under that improper map the quat
     *  axis part is swapped AND negated, w preserved:
     *      q_skel = (-qx_mhr, -qz_mhr, -qy_mhr, qw_mhr)                      */
    inline FQuat ToUEQuatFromMHR(const FQuat& InMHRQuat)
    {
        return FQuat(-InMHRQuat.X, -InMHRQuat.Z, -InMHRQuat.Y, InMHRQuat.W);
    }

    /** MHR root-position delta (meters, agent frame, pelvis-centred) →
     *  SK_GestureSyncSource root-local translation delta.
     *
     *  SOLVED 2026-07-02 by scripts/ue_solve_mhr_axis.py (48-perm × scale
     *  solve, winner "x,z,y" @ scale 100.0 err 0.0, runner-up 16.0 — see
     *  ue-mcp-artifacts/mhr_axis_solve.json). UNITS (single documented
     *  place, corrected 2026-07-03 by the Task 25 smoke): the import carries
     *  the m→cm conversion as a ×100 SCALE on the 'root' bone (child bind
     *  translations are METERS in bone-local space; component space is cm).
     *  Root's own translation lives in its PARENT (component) space — i.e.
     *  centimeters — so the wire's meters delta converts to CENTIMETERS here;
     *  the solver measured the ×100 directly against component space. The
     *  receiver preserves each bone's ref-pose scale when publishing
     *  (EstuaryBodyAnimationReceiver.cpp) so root's ×100 survives LiveLink.
     *  Y↔Z swap + m→cm:
     *      (x, y, z)_skel = (x_mhr, z_mhr, y_mhr) × 100  — CENTIMETERS.      */
    inline FVector ToUEHipsFromMHR(const FVector& InMHRHipsMeters)
    {
        return FVector(InMHRHipsMeters.X * 100.0f, InMHRHipsMeters.Z * 100.0f,
                       InMHRHipsMeters.Y * 100.0f);
    }

    /** Diagnostic categories. The 4 conversion failure modes plus None.
     *  When the CoordRoundTrip fixture gate reports a quat-error mismatch,
     *  DiagnoseQuatResidual returns the mode whose alternative conversion
     *  variant matches the observed quat within Epsilon. */
    enum class EConversionFailureMode : uint8
    {
        None,
        AxisSwap,
        SignFlip,
        Handedness,
        Roll,
    };

    /** Diagnostic helper (~30 LOC): given Expected and Observed quaternions, return
     *  the mode whose alternative conversion variant matches Observed within Epsilon.
     *  Exported because impl is non-inline. */
    ESTUARY_API EConversionFailureMode DiagnoseQuatResidual(
        const FQuat& Expected, const FQuat& Observed, float Epsilon = 1e-4f);

    /** Stringify the failure mode for human-readable test failure logs. */
    ESTUARY_API const TCHAR* EConversionFailureModeToString(EConversionFailureMode Mode);
}
