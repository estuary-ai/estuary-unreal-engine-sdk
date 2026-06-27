// EstuaryUserPosMathTests.cpp — FAutomationTestBase tests for the pure-math
// agent-local floor projection in EstuaryUserPosMath.h (SDK_CONTRACT §user_pos).
//
// HOW TO RUN:
//   1. Open your UE 5.6+ project in the editor.
//   2. Window > Session Frontend > Automation tab.
//   3. Filter by "Estuary.Body.UserPos".
//   4. Select tests below and click "Run Tests".
//
// These tests pin the axis convention whose inversion shipped as a gaze bug
// (audit 2026-06-06): the wire payload is { x = agent's RIGHT, y = agent's FORWARD }
// in meters, NOT (forward, right). Mirror of EstuaryBodyFrameTransformTests.cpp.
// Pitfall 23: IWYU only.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "EstuaryUserPosMath.h"

// ---------------------------------------------------------------------------
// Test 1: a user directly in FRONT of the agent maps to wire y (forward), not x.
//   Agent yaw = 0 (forward = UE +X). Delta = (+200 cm, 0, 0) = 2 m straight ahead.
//   Expect: x ≈ 0 (no lateral offset), y ≈ +2.0 m (forward).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryUserPosForwardMapsToWireY,
    "Estuary.Body.UserPos.ForwardMapsToWireY",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryUserPosForwardMapsToWireY::RunTest(const FString& /*Params*/)
{
    float X = -999.0f;
    float Y = -999.0f;
    EstuaryUserPosMath::ProjectToGestureSyncFrame(FVector(200.0f, 0.0f, 0.0f), 0.0f, X, Y);

    TestTrue(TEXT("User in front: wire x (left) is ~0"), FMath::IsNearlyZero(X, 1e-4f));
    TestTrue(TEXT("User in front: wire y (forward) is +2.0 m"), FMath::IsNearlyEqual(Y, 2.0f, 1e-4f));
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 2: a user to the agent's RIGHT maps to wire x = NEGATIVE (x is the agent's LEFT).
//   Agent yaw = 0 (right = UE +Y). Delta = (0, +200 cm, 0) = 2 m to the right.
//   Expect: x ≈ -2.0 m (user on the right = negative LEFT-axis), y ≈ 0.
//   This is the exact assertion that fails if (forward, right) is emitted instead.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryUserPosRightMapsToWireX,
    "Estuary.Body.UserPos.RightMapsToWireX",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryUserPosRightMapsToWireX::RunTest(const FString& /*Params*/)
{
    float X = -999.0f;
    float Y = -999.0f;
    EstuaryUserPosMath::ProjectToGestureSyncFrame(FVector(0.0f, 200.0f, 0.0f), 0.0f, X, Y);

    TestTrue(TEXT("User to the right: wire x (left-positive) is -2.0 m"), FMath::IsNearlyEqual(X, -2.0f, 1e-4f));
    TestTrue(TEXT("User to the right: wire y (forward) is ~0"), FMath::IsNearlyZero(Y, 1e-4f));
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 3: yaw is applied — a user at world +Y with the agent yawed 90° (so the
// agent faces world +Y) is directly AHEAD of the agent.
//   yaw = 90° → LocalForward = Delta·(cos90, sin90) = Delta.Y; LocalRight = Delta·(-sin90, cos90) = -Delta.X.
//   Delta = (0, +300 cm, 0) → x ≈ 0, y ≈ +3.0 m.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryUserPosYawRotation,
    "Estuary.Body.UserPos.YawRotation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryUserPosYawRotation::RunTest(const FString& /*Params*/)
{
    float X = -999.0f;
    float Y = -999.0f;
    EstuaryUserPosMath::ProjectToGestureSyncFrame(FVector(0.0f, 300.0f, 0.0f), 90.0f, X, Y);

    TestTrue(TEXT("Agent yawed 90° toward the user: wire x (left) is ~0"), FMath::IsNearlyZero(X, 1e-3f));
    TestTrue(TEXT("Agent yawed 90° toward the user: wire y (forward) is +3.0 m"), FMath::IsNearlyEqual(Y, 3.0f, 1e-3f));
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
