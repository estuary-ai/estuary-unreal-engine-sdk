// EstuaryAmplitudeEnvelopeTests.cpp — FAutomationTestBase tests for EstuaryAmplitude::ComputeJawWeight.
//
// HOW TO RUN:
//   1. Open your UE 5.6+ project in the editor.
//   2. Window > Session Frontend > Automation tab.
//   3. Filter by "Estuary.Amplitude".
//   4. Select tests below and click "Run Tests".
//
// Quick task 260428-365 — amplitude jaw layer (T2).
// Pitfall 23: IWYU only.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "EstuaryAmplitudeMath.h"

// ---------------------------------------------------------------------------
// Test 1: zero envelope returns 0
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryAmplitudeJawZeroEnvelope,
    "Estuary.Amplitude.JawZeroEnvelope",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryAmplitudeJawZeroEnvelope::RunTest(const FString& /*Params*/)
{
    const float Result = EstuaryAmplitude::ComputeJawWeight(0.0f, 100.0, 100.0);  // fresh, env=0
    TestEqual(TEXT("Zero envelope returns 0"), Result, 0.0f);
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 2: full-scale envelope (1.0) returns kJawMax = 0.6
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryAmplitudeJawFullScale,
    "Estuary.Amplitude.JawFullScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryAmplitudeJawFullScale::RunTest(const FString& /*Params*/)
{
    const float Result = EstuaryAmplitude::ComputeJawWeight(1.0f, 100.0, 100.0);  // fresh, env=1
    TestEqual(TEXT("Full-scale envelope returns kJawMax (0.6)"),
              Result, EstuaryAmplitude::kJawMax);
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 3: mid-scale envelope (0.5) returns pow(0.5, 0.7) * 0.6 ≈ 0.369
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryAmplitudeJawMidScale,
    "Estuary.Amplitude.JawMidScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryAmplitudeJawMidScale::RunTest(const FString& /*Params*/)
{
    // pow(0.5, 0.7) ≈ 0.6156; * 0.6 ≈ 0.3693
    const float Result   = EstuaryAmplitude::ComputeJawWeight(0.5f, 100.0, 100.0);
    const float Expected = FMath::Pow(0.5f, EstuaryAmplitude::kAmpCurveExp) *
                           EstuaryAmplitude::kJawMax;
    TestTrue(TEXT("Mid-scale envelope matches pow(env, 0.7) * 0.6"),
             FMath::IsNearlyEqual(Result, Expected, 1e-5f));
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 4: staleness gate — envelope >100ms old returns 0 even if non-zero value
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryAmplitudeJawStaleGate,
    "Estuary.Amplitude.JawStaleGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryAmplitudeJawStaleGate::RunTest(const FString& /*Params*/)
{
    // Now=100.0, LastUpdate=99.5 (500ms ago) → staleness gate (>100ms) trips → return 0
    // even with a non-zero envelope value.
    const float Result = EstuaryAmplitude::ComputeJawWeight(0.8f, 100.0, 99.5);
    TestEqual(TEXT("Stale envelope (>100ms old) returns 0"), Result, 0.0f);

    // Boundary: exactly at staleness threshold (100ms) → still considered FRESH (gate is strict >).
    const float ResultBoundary = EstuaryAmplitude::ComputeJawWeight(
        0.8f, 100.0, 100.0 - EstuaryAmplitude::kEnvelopeStaleSec);
    TestTrue(TEXT("Envelope at exact staleness threshold is fresh (returns non-zero)"),
             ResultBoundary > 0.0f);
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
