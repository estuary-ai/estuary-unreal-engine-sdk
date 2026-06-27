// EstuaryBodyFrameTransformTests.cpp — FAutomationTestBase tests for the
// pure-math SMPL-X→UE conversion helpers in EstuaryBodyFrameTransform.h.
//
// HOW TO RUN:
//   1. Open your UE 5.6+ project in the editor.
//   2. Window > Session Frontend > Automation tab.
//   3. Filter by "Estuary.Body.FrameTransform".
//   4. Select tests below and click "Run Tests".
//
// Phase 4 (UE-05 unit tests). Mirror of EstuaryAmplitudeEnvelopeTests.cpp.
// Pitfall 23: IWYU only.
//
// NOTE on test 2 (AxisSwap): the expected quat is the result the LOCKED conversion
// formula `q_ue = (-qz_smplx, -qx_smplx, -qy_smplx, qw_smplx)` produces — NOT the
// naive "+90° about UE +Z" intuition. The formula's negation step flips the rotation
// sense (the resulting axis is UE -Z, equivalent to -90° about UE +Z). The UE-06
// 6-pose math gate is the empirical falsifier that confirms whether this is the correct
// physical interpretation against GestureSync ground-truth poses. The unit test here
// verifies formula correctness, not physical interpretation.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "EstuaryBodyFrameTransform.h"

// ---------------------------------------------------------------------------
// Test 1: identity quat in → identity quat out (the (0,0,0,1) invariant).
//   q_in  = (0,0,0,1)
//   q_out = (-0, -0, -0, 1) = (0,0,0,1)  (sign flip of zero is zero)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyFrameTransformIdentityQuat,
    "Estuary.Body.FrameTransform.IdentityQuat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyFrameTransformIdentityQuat::RunTest(const FString& /*Params*/)
{
    const FQuat Result = EstuaryBodyFrameTransform::ToUEQuatFromGestureSync(FQuat::Identity);
    TestTrue(TEXT("Identity quat in produces identity-equivalent quat out"),
             FQuat::ErrorAutoNormalize(Result, FQuat::Identity) < 1e-4f);
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 2: Y-mirror conjugation — 90° rotation about SMPL-X +Y stays a 90° rotation
// about skeleton-local +Y (qy preserved, qx/qz negated) per the 2026-06-09 formula.
//   q_in  = FQuat(FVector(0,1,0), HALF_PI)     // SMPL-X axis (0,1,0)
//         = (0, sin(45°), 0, cos(45°)) = (0, 0.7071, 0, 0.7071)
//   q_out = (-q_in.X, q_in.Y, -q_in.Z, q_in.W)
//         = (0, 0.7071, 0, 0.7071)
//         = FQuat(FVector(0,1,0), HALF_PI)     // unchanged: axis lies in the mirror plane normal
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyFrameTransformAxisSwap,
    "Estuary.Body.FrameTransform.AxisSwap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyFrameTransformAxisSwap::RunTest(const FString& /*Params*/)
{
    const FQuat Input(FVector(0.0f, 1.0f, 0.0f), HALF_PI);
    const FQuat Result = EstuaryBodyFrameTransform::ToUEQuatFromGestureSync(Input);

    // Y-mirror formula: q_skel = (-qx, qy, -qz, qw). Input (0, sin45, 0, cos45) →
    // (-0, sin45, -0, cos45) = rotation about +Y by 90° (unchanged).
    const FQuat Expected(FVector(0.0f, 1.0f, 0.0f), HALF_PI);

    TestTrue(TEXT("90° SMPL-X +Y rotation maps to 90° +Y rotation under Y-mirror formula"),
             FQuat::ErrorAutoNormalize(Result, Expected) < 1e-4f);
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 3: hips Y-mirror, meter-passthrough (consuming skeleton is meter-scaled;
// the SMPLX-male node carries the ×100 — publishing cm double-scales).
//   v_in  = FVector(0.0f, 1.0f, 0.0f)         // SMPL-X meters
//   v_out = (v_in.X, -v_in.Y, v_in.Z) = (0, -1, 0)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyFrameTransformHipsMetersToCm,
    "Estuary.Body.FrameTransform.HipsMetersToCm",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyFrameTransformHipsMetersToCm::RunTest(const FString& /*Params*/)
{
    const FVector Input(0.0f, 1.0f, 0.0f);
    const FVector Result = EstuaryBodyFrameTransform::ToUEHipsFromGestureSync(Input);
    const FVector Expected(0.0f, -1.0f, 0.0f);
    TestTrue(TEXT("(0, 1, 0) SMPL-X meters → (0, -1, 0) skeleton-local meters (Y-mirror, no scale)"),
             Result.Equals(Expected, 1e-4f));

    // Sanity: kHipsMetersToCm constant is exactly 100.0f in this single source-of-truth.
    TestEqual(TEXT("kHipsMetersToCm constant equals 100.0f"),
              EstuaryBodyFrameTransform::kHipsMetersToCm, 100.0f);
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 4: roll offset — passing identity quat with RollOffsetDeg=90 produces a 90°
// rotation about UE +X (ForwardVector — bone-forward axis convention).
//   ToUEQuatFromGestureSync(Identity) = Identity
//   Q = Identity * FQuat(ForwardVector, deg2rad(90)) = FQuat(ForwardVector, deg2rad(90))
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyFrameTransformRollOffset,
    "Estuary.Body.FrameTransform.RollOffset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyFrameTransformRollOffset::RunTest(const FString& /*Params*/)
{
    const FQuat Result = EstuaryBodyFrameTransform::ToUEQuatFromGestureSync(FQuat::Identity, 90.0f);
    const FQuat Expected(FVector::ForwardVector, HALF_PI);
    TestTrue(TEXT("Identity quat with RollOffsetDeg=90 produces 90° rotation about UE +X"),
             FQuat::ErrorAutoNormalize(Result, Expected) < 1e-4f);

    // Boundary: RollOffsetDeg=0 must produce the same as the no-roll overload.
    const FQuat NoRollResult = EstuaryBodyFrameTransform::ToUEQuatFromGestureSync(FQuat::Identity, 0.0f);
    const FQuat NoRollExpected = EstuaryBodyFrameTransform::ToUEQuatFromGestureSync(FQuat::Identity);
    TestTrue(TEXT("RollOffsetDeg=0 matches no-roll overload (KINDA_SMALL_NUMBER guard)"),
             FQuat::ErrorAutoNormalize(NoRollResult, NoRollExpected) < 1e-6f);
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 5: DiagnoseQuatResidual — AxisSwap mode.
// Construct an Observed that matches the AxisSwap variant of Expected (Y↔Z swap
// without any negation). The diagnostic should name AxisSwap.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyFrameTransformDiagnoseAxisSwap,
    "Estuary.Body.FrameTransform.DiagnoseAxisSwap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyFrameTransformDiagnoseAxisSwap::RunTest(const FString& /*Params*/)
{
    // Pick an asymmetric Expected so the variants are distinguishable from each other.
    const FQuat Expected(0.1f, 0.2f, 0.3f, 0.927f);
    // AxisSwap variant = (X, Z, Y, W) — Y↔Z swap, no negation.
    const FQuat Observed(Expected.X, Expected.Z, Expected.Y, Expected.W);

    const auto Mode = EstuaryBodyFrameTransform::DiagnoseQuatResidual(Expected, Observed);
    TestEqual(TEXT("AxisSwap variant residual is diagnosed as AxisSwap"),
              static_cast<int32>(Mode),
              static_cast<int32>(EstuaryBodyFrameTransform::EConversionFailureMode::AxisSwap));
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 6: DiagnoseQuatResidual — SignFlip mode.
// Observed matches the SignFlip variant of Expected (negate X only).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyFrameTransformDiagnoseSignFlip,
    "Estuary.Body.FrameTransform.DiagnoseSignFlip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyFrameTransformDiagnoseSignFlip::RunTest(const FString& /*Params*/)
{
    const FQuat Expected(0.1f, 0.2f, 0.3f, 0.927f);
    const FQuat Observed(-Expected.X, Expected.Y, Expected.Z, Expected.W);

    const auto Mode = EstuaryBodyFrameTransform::DiagnoseQuatResidual(Expected, Observed);
    TestEqual(TEXT("SignFlip variant residual is diagnosed as SignFlip"),
              static_cast<int32>(Mode),
              static_cast<int32>(EstuaryBodyFrameTransform::EConversionFailureMode::SignFlip));
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 7: DiagnoseQuatResidual — Handedness mode.
// Observed matches the Handedness variant of Expected (negate all of xyz).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyFrameTransformDiagnoseHandedness,
    "Estuary.Body.FrameTransform.DiagnoseHandedness",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyFrameTransformDiagnoseHandedness::RunTest(const FString& /*Params*/)
{
    const FQuat Expected(0.1f, 0.2f, 0.3f, 0.927f);
    const FQuat Observed(-Expected.X, -Expected.Y, -Expected.Z, Expected.W);

    const auto Mode = EstuaryBodyFrameTransform::DiagnoseQuatResidual(Expected, Observed);
    TestEqual(TEXT("Handedness variant residual is diagnosed as Handedness"),
              static_cast<int32>(Mode),
              static_cast<int32>(EstuaryBodyFrameTransform::EConversionFailureMode::Handedness));
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
