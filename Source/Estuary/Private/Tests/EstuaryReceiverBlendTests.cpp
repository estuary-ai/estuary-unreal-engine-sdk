// EstuaryReceiverBlendTests.cpp — FAutomationTestBase tests for EstuaryBlend::ComputeCrossfadeAlpha.
//
// HOW TO RUN:
//   1. Open your UE 5.6+ project in the editor.
//   2. Window > Session Frontend > Automation tab.
//   3. Filter by "Estuary.Blend".
//   4. Select tests below and click "Run Tests".
//
// Quick task 260428-365 — amplitude jaw layer (T3).
// Pitfall 23: IWYU only.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "EstuaryReceiverBlendMath.h"

// ---------------------------------------------------------------------------
// Test 1: No A2F seen → α = 0 regardless of NowSec or A2FCrossfadeMs
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBlendNoA2fSeen,
    "Estuary.Blend.NoA2fSeen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBlendNoA2fSeen::RunTest(const FString& /*Params*/)
{
    // FirstA2fSec = -1 → α = 0 regardless of NowSec or A2FCrossfadeMs.
    const float A = EstuaryBlend::ComputeCrossfadeAlpha(100.0, -1.0, -1.0, 200.0f);
    TestEqual(TEXT("No A2F seen → α = 0"), A, 0.0f);
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 2: Fade-in halfway through ramp-in window → α ≈ 0.5
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBlendFadeInMidway,
    "Estuary.Blend.FadeInMidway",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBlendFadeInMidway::RunTest(const FString& /*Params*/)
{
    // A2FCrossfadeMs = 200 → halfXfade = 100ms = 0.1s.
    // Now = 100.05s, FirstA2fSec = 100.0s → elapsed = 0.05s = halfway through fade-in.
    const float A = EstuaryBlend::ComputeCrossfadeAlpha(100.05, 100.0, -1.0, 200.0f);
    TestTrue(TEXT("Fade-in halfway: α ≈ 0.5"), FMath::IsNearlyEqual(A, 0.5f, 1e-4f));
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 3: Past fade-in, still in hold (no end yet) → α = 1
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBlendHoldFullCover,
    "Estuary.Blend.HoldFullCover",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBlendHoldFullCover::RunTest(const FString& /*Params*/)
{
    // Past fade-in (elapsed > halfXfade), still in hold (LastA2fEndSec = -1).
    const float A = EstuaryBlend::ComputeCrossfadeAlpha(100.5, 100.0, -1.0, 200.0f);
    TestEqual(TEXT("Past fade-in, still covering → α = 1"), A, 1.0f);
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 4: Fade-out halfway through ramp-out window → α ≈ 0.5
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBlendFadeOutMidway,
    "Estuary.Blend.FadeOutMidway",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBlendFadeOutMidway::RunTest(const FString& /*Params*/)
{
    // halfXfade = 0.1s. Now = 100.55s, LastA2fEndSec = 100.5s → elapsed = 0.05s into fade-out.
    const float A = EstuaryBlend::ComputeCrossfadeAlpha(100.55, 100.0, 100.5, 200.0f);
    TestTrue(TEXT("Fade-out halfway: α ≈ 0.5"), FMath::IsNearlyEqual(A, 0.5f, 1e-4f));
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 5: Past fade-out window → α = 0
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBlendFadeOutComplete,
    "Estuary.Blend.FadeOutComplete",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBlendFadeOutComplete::RunTest(const FString& /*Params*/)
{
    // Past fade-out (elapsed > halfXfade after LastA2fEndSec).
    const float A = EstuaryBlend::ComputeCrossfadeAlpha(101.0, 100.0, 100.5, 200.0f);
    TestEqual(TEXT("Past fade-out → α = 0"), A, 0.0f);
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
