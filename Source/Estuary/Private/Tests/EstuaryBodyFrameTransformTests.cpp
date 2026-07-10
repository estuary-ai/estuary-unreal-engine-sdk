// EstuaryBodyFrameTransformTests.cpp — FAutomationTestBase tests for the
// pure-math MHR→UE conversion helpers in EstuaryBodyFrameTransform.h.
//
// HOW TO RUN:
//   1. Open the UE project in the editor.
//   2. Window > Session Frontend > Automation tab.
//   3. Filter by "Estuary.Body.FrameTransform".
//   4. Select tests below and click "Run Tests".
//   Headless: UnrealEditor-Cmd.exe <uproject>
//     -ExecCmds="Automation RunTests Estuary.Body.FrameTransform; Quit"
//     -unattended -nullrhi -nosplash -nopause -log
//
// SOLVED PIN (2026-07-02, Phase 5 Task 20 re-pin): these tests pin the SOLVED
// MHR component maps — quat "-x,-z,-y,w" and hips "x,z,y" × 100 (cm) — from
// the 48-perm FK ground-truth solve against the imported SK_GestureSyncSource
// (scripts/ue_solve_mhr_axis.py; quat winner err 0.0 vs runner-up 3.016,
// hips err 0.0 vs runner-up 16.0 — ue-mcp-artifacts/mhr_axis_solve.json).
// The Phase 4 provisional Y-mirror did NOT survive the solve: the MHR→UE
// frame change is a pure Y↔Z swap (RH Y-up → LH Z-up). Test NAMES are kept
// from the provisional pin for harness stability; the pinned VALUES below
// are the solved map. Reimporting the FBX with different axis settings
// invalidates these — re-run the solver and re-pin.
//
// Pitfall 23: IWYU only.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "EstuaryBodyFrameTransform.h"

// ---------------------------------------------------------------------------
// Test 1: identity quat in → identity quat out (the (0,0,0,1) invariant).
//   q_in  = (0,0,0,1)
//   q_out = (-0, 0, -0, 1) = (0,0,0,1)  (sign flip of zero is zero)
// NOTE: identity is a FORMULA invariant, not the rest pose — on the MHR rig
// identity != rest (A-pose bind locals).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyFrameTransformIdentityQuat,
    "Estuary.Body.FrameTransform.IdentityQuat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyFrameTransformIdentityQuat::RunTest(const FString& /*Params*/)
{
    const FQuat Result = EstuaryBodyFrameTransform::ToUEQuatFromMHR(FQuat::Identity);
    TestTrue(TEXT("Identity quat in produces identity-equivalent quat out"),
             FQuat::ErrorAutoNormalize(Result, FQuat::Identity) < 1e-4f);
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 2: up-axis mapping under the SOLVED Y↔Z swap — a 90° rotation about
// MHR +Y (up) maps to a 90° rotation about UE −Z (up axis, direction sign
// encoding the RH→LH handedness flip).
//   q_in  = FQuat(FVector(0,1,0), HALF_PI) = (0, 0.7071, 0, 0.7071)
//   q_out = (-qx, -qz, -qy, qw) = (0, 0, -0.7071, 0.7071)
//         = FQuat(FVector(0,0,-1), HALF_PI)
// (Name kept from the provisional pin for harness stability.)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyFrameTransformYMirrorPreservesYAxis,
    "Estuary.Body.FrameTransform.YMirrorPreservesYAxis",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyFrameTransformYMirrorPreservesYAxis::RunTest(const FString& /*Params*/)
{
    const FQuat Input(FVector(0.0f, 1.0f, 0.0f), HALF_PI);
    const FQuat Result = EstuaryBodyFrameTransform::ToUEQuatFromMHR(Input);
    const FQuat Expected(FVector(0.0f, 0.0f, -1.0f), HALF_PI);
    TestTrue(TEXT("90° MHR +Y (up) rotation maps to 90° UE -Z rotation under the solved Y↔Z swap"),
             FQuat::ErrorAutoNormalize(Result, Expected) < 1e-4f);
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 3: exact-component pin of the SOLVED quat map "-x,-z,-y,w" on an
// asymmetric input (Phase 5 Task 20 re-pin; name kept for harness stability).
//   q_in  = (0.1, 0.2, 0.3, 0.927)
//   q_out = (-0.1, -0.3, -0.2, 0.927)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyFrameTransformProvisionalComponentPin,
    "Estuary.Body.FrameTransform.ProvisionalComponentPin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyFrameTransformProvisionalComponentPin::RunTest(const FString& /*Params*/)
{
    const FQuat Input(0.1f, 0.2f, 0.3f, 0.927f);
    const FQuat Result = EstuaryBodyFrameTransform::ToUEQuatFromMHR(Input);
    TestTrue(TEXT("X component == -qx"), FMath::IsNearlyEqual((float)Result.X, -0.1f,   1e-6f));
    TestTrue(TEXT("Y component == -qz"), FMath::IsNearlyEqual((float)Result.Y, -0.3f,   1e-6f));
    TestTrue(TEXT("Z component == -qy"), FMath::IsNearlyEqual((float)Result.Z, -0.2f,   1e-6f));
    TestTrue(TEXT("W component ==  qw"), FMath::IsNearlyEqual((float)Result.W,  0.927f, 1e-6f));
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 4: hips SOLVED map — "x,z,y" × 100: Y↔Z swap, meters → CENTIMETERS
// (this import bakes cm into bone-local translations; solver measured the
// ×100 directly — see header doc; name kept for harness stability).
//   v_in  = (0.25, 1.0, -0.5) meters
//   v_out = (25, -50, 100) centimeters
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyFrameTransformHipsMetersYMirror,
    "Estuary.Body.FrameTransform.HipsMetersYMirror",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyFrameTransformHipsMetersYMirror::RunTest(const FString& /*Params*/)
{
    const FVector Input(0.25f, 1.0f, -0.5f);
    const FVector Result = EstuaryBodyFrameTransform::ToUEHipsFromMHR(Input);
    const FVector Expected(25.0f, -50.0f, 100.0f);
    TestTrue(TEXT("(0.25, 1, -0.5) MHR meters → (25, -50, 100) skeleton-local cm (Y↔Z swap, ×100)"),
             Result.Equals(Expected, 1e-4f));
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
