// EstuaryBodyFrameTransform.h — pure-math SMPL-X→UE coordinate conversion (UE-05).
// Pitfall 23: #pragma once, IWYU only, no CoreMinimal.h, no Engine.h.
//
// Mirror of EstuaryAmplitudeMath.h pattern: pure-math, header-only namespace,
// `static constexpr` constants + namespaced inline helpers; no UObject deps.
// One non-inline function (DiagnoseQuatResidual) annotated with ESTUARY_API
// so its implementation can live in a tiny .cpp file.
//
// SMPL-X→UE conversion (locked):
//   - SMPL-X frame: right-handed, +Y up, +Z forward, meters
//   - UE frame:     left-handed,  +Z up, +X forward, centimeters
// The conversion involves both an axis remap (z_smplx, x_smplx, y_smplx) and a
// handedness flip (negate vector components). Combined formula for quaternions:
//   q_ue = (-qz_smplx, -qx_smplx, -qy_smplx, qw_smplx)
// Positions only need the axis remap + meters→centimeters scaling (no handedness
// flip applies to positions).
//
// The UE-06 6-pose math test is the empirical falsifier for this
// basis pairing. If UE-06 fails, DiagnoseQuatResidual names which of the four
// failure modes (axis swap, sign flip, handedness, roll) the residual matches.

#pragma once

#include "Math/UnrealMathUtility.h"
#include "Math/Quat.h"
#include "Math/Vector.h"

namespace EstuaryBodyFrameTransform
{
    /** SMPL-X meters to UE centimeters. Locked in this single constant (UE-05).
     *  Hips × 100 m→cm conversion lives in EXACTLY ONE place — this constant is the
     *  single source of truth and must not be re-defined anywhere else. */
    constexpr float kHipsMetersToCm = 100.0f;

    /** Pure conversion: SMPL-X local quat (RH +Y up +Z forward) → SK_GestureSyncSource bone-local quat.
     *  The published quats are consumed by LiveLinkPose on SK_GestureSyncSource (official SMPL-X FBX,
     *  identity bone-local binds). The UE FBX importer left the skeleton's sub-node frame as a
     *  Y-MIRROR of the SMPL-X frame (RH→LH handedness flip negates Y; head sits at -Y under the
     *  `SMPLX-male` axis node, which performs the uprighting into component space). A rotation
     *  conjugated through a Y-mirror keeps qy and negates qx/qz (axis is a pseudovector):
     *  q_skel = (-qx_smplx, qy_smplx, -qz_smplx, qw_smplx).
     *  Derived empirically 2026-06-09 by brute-force FK ground-truth solve (48 signed perms,
     *  7 test rotations, winner error 0.0; prior formula (-qz,-qx,-qy,qw) error 11.97 — it was
     *  authored for the pre-2026-05-05 hand-built skeleton and never falsified in-editor).
     *  NOTE: this map is specific to the SK_GestureSyncSource FBX import; reimporting the skeleton
     *  with different axis settings invalidates it (re-run the solver in that case). */
    inline FQuat ToUEQuatFromGestureSync(const FQuat& InGestureSyncQuat)
    {
        return FQuat(-InGestureSyncQuat.X, InGestureSyncQuat.Y, -InGestureSyncQuat.Z, InGestureSyncQuat.W);
    }

    /** Pure conversion: SMPL-X hips position (meters) → SK_GestureSyncSource pelvis-local delta.
     *  Published as a pelvis LOCAL translation under the `SMPLX-male` axis node, so the target
     *  frame is the same Y-mirrored SMPL frame as the quats (positions mirror directly):
     *  (x, y, z)_skel = (x_smplx, -y_smplx, z_smplx) — in METERS, NOT cm. The SK_GestureSyncSource
     *  import keeps file units (meters) in bone-local translations and carries the ×100 on the
     *  SMPLX-male node's scale; publishing cm here gets double-scaled and the source mesh flies
     *  ±10m around the level (measured 2026-06-09, pie_pose_samples.json).
     *  Same empirical derivation/caveat as ToUEQuatFromGestureSync (prior (z,x,y)×100 map). */
    inline FVector ToUEHipsFromGestureSync(const FVector& InGestureSyncHipsMeters)
    {
        return FVector(InGestureSyncHipsMeters.X, -InGestureSyncHipsMeters.Y, InGestureSyncHipsMeters.Z);
    }

    /** Combined per-bone conversion + optional roll offset.
     *  RollOffsetDeg defaults to 0; only set in DT_GestureSyncBoneRemap rows where the MetaHuman
     *  bone has a baked-in twist relative to its SMPL-X counterpart (rare; populate empirically
     *  via 6-pose test residuals). The roll is applied AFTER the basis conversion, about the
     *  UE bone-local +X axis (UE convention: bone-forward axis = +X). */
    inline FQuat ToUEQuatFromGestureSync(const FQuat& InGestureSyncQuat, float RollOffsetDeg)
    {
        FQuat Q = ToUEQuatFromGestureSync(InGestureSyncQuat);
        if (FMath::Abs(RollOffsetDeg) > KINDA_SMALL_NUMBER)
        {
            // Per-bone roll about UE local +X (bone-forward axis convention).
            Q = Q * FQuat(FVector::ForwardVector, FMath::DegreesToRadians(RollOffsetDeg));
        }
        return Q;
    }

    /** Bone-local axis-label remap: GestureSync skeleton (X-primary, X=bone-direction) → MH skeleton
     *  (Y-primary, Y=bone-direction). Applied AFTER ToUEQuatFromGestureSync when feeding bone rotations
     *  into a Y-primary target rig such as MetaHuman.
     *
     *  Why: SK_GestureSyncSource bone-local frames have X along the bone (visible in IKR editor: red
     *  axis along the arm), while MetaHuman bone-local frames have Y along the bone (green axis
     *  along the arm). IKR's COPY_LOCAL mode passes (qx, qy, qz, qw) tuples unchanged from
     *  source-local frame to target-local frame. Without this remap, a "twist around bone"
     *  rotation encoded as a pure-X rotation in GestureSync gets interpreted as a pure-X rotation in
     *  MH — but MH's X is the down/caudal axis, not the bone-direction axis. Result: right arm
     *  shoots overhead, left arm has 90° wrist twist (observed during cross-skeleton bring-up, 2026-05-04).
     *
     *  Math: 180° rotation around (1,1,0)/√2 in bone-local frame, applied via quaternion
     *  conjugation. Closed-form on (qx, qy, qz, qw): (qy, qx, -qz, qw). Effect on basis vectors:
     *  X↔Y swap with Z negation (preserves right-handedness). Self-inverse: applying twice
     *  returns identity. */
    inline FQuat ApplyLocalAxisRemap(const FQuat& InUEQuat)
    {
        return FQuat(InUEQuat.Y, InUEQuat.X, -InUEQuat.Z, InUEQuat.W);
    }

    /** INVERSE of ToUEQuatFromGestureSync — UE local quat (LH +Z up +X forward) → SMPL-X local quat (RH +Y up +Z forward).
     *  Used by SDK clients that read bind quats from a UE skeletal mesh and need to ship them
     *  to GestureSync in the canonical frame. Reversing the axis-swap perm (X←-Z, Y←-X, Z←-Y) gives
     *  (X←-Y, Y←-Z, Z←-X); handedness flip negates vector components, w preserved.
     *  Result: q_gestureSync = (-qy_ue, -qz_ue, -qx_ue, qw_ue).
     *  Round-trip identity: ToGestureSyncQuatFromUE(ToUEQuatFromGestureSync(q)) ≈ q (modulo float precision).
     *  See the "Bind extraction" derivation in the body-animation design notes. */
    inline FQuat ToGestureSyncQuatFromUE(const FQuat& InUEQuat)
    {
        return FQuat(-InUEQuat.Y, -InUEQuat.Z, -InUEQuat.X, InUEQuat.W);
    }

    /** Diagnostic categories (locked). The 4 SMPL-X→UE failure modes plus None.
     *  When UE-06 reports a quat-error mismatch, DiagnoseQuatResidual returns the mode
     *  whose alternative conversion variant matches the observed quat within Epsilon. */
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
