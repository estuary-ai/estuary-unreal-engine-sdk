// EstuaryAnimationFrameBufferTests.cpp
// FAutomationTestBase unit tests for FAnimationFrameBuffer.
//
// HOW TO RUN:
//   1. Open your UE 5.5 project in the editor.
//   2. Window > Session Frontend > Automation tab.
//   3. Filter by "Estuary.Animation" or "Estuary".
//   4. Select the tests below and click "Run Tests".
//
// FAnimationFrameBuffer is a plain C++ struct (not a UObject) and therefore
// can be instantiated directly on the stack without NewObject<>. These tests
// run in any ApplicationContextMask (editor or game).
//
// Pitfall 23: #pragma once not needed in .cpp; IWYU only.
// Pitfall 10: Tests exercise the interpolation, drift, clear, and capacity-cap invariants
//             that are critical for the 30-Hz-to-60+-Hz upsample correctness guarantee.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "EstuaryAnimationFrameBuffer.h"
#include "EstuaryTypes.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace AnimFrameBufferTestHelpers
{
    /** Build a minimal FEstuaryBotAnimation frame at the given timestamp
     *  with synthetic blendshape weights. Weight = (CurveIndex / 51.0f) * amplitude
     *  to produce a non-trivial, verifiable pattern. */
    static FEstuaryBotAnimation MakeFrame(float TimeCodeSec, float Amplitude = 1.0f)
    {
        FEstuaryBotAnimation Frame;
        Frame.TimeCodeSec = TimeCodeSec;
        Frame.MessageId   = TEXT("test-msg-001");
        Frame.bIsFinal = false;

        // Standard ARKit-52 names (subset used for testing).
        static const TCHAR* TestCurveNames[] = {
            TEXT("jawOpen"),
            TEXT("mouthSmileLeft"),
            TEXT("mouthSmileRight"),
            TEXT("eyeBlinkLeft"),
            TEXT("eyeBlinkRight"),
        };
        constexpr int32 NCurves = UE_ARRAY_COUNT(TestCurveNames);

        for (int32 i = 0; i < NCurves; ++i)
        {
            Frame.Weights.Add(TestCurveNames[i],
                FMath::Clamp((static_cast<float>(i) / (NCurves - 1)) * Amplitude, 0.0f, 1.0f));
        }
        return Frame;
    }

    /** Returns true if A and B are within Tolerance of each other. */
    static bool NearlyEqual(float A, float B, float Tolerance = 1e-4f)
    {
        return FMath::Abs(A - B) <= Tolerance;
    }
} // namespace AnimFrameBufferTestHelpers

// ---------------------------------------------------------------------------
// Test 1: Push + GetInterpolatedWeights between two frames
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryAnimBufInterpolation,
    "Estuary.Animation.FrameBuffer.Interpolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FEstuaryAnimBufInterpolation::RunTest(const FString& Parameters)
{
    FAnimationFrameBuffer Buf;

    // Push two frames: t=0.0 (all zeros) and t=1.0 (all ones on jawOpen).
    FEstuaryBotAnimation FrameA = AnimFrameBufferTestHelpers::MakeFrame(0.0f, 0.0f);
    FEstuaryBotAnimation FrameB = AnimFrameBufferTestHelpers::MakeFrame(1.0f, 1.0f);

    // Override jawOpen specifically for a clean interpolation check.
    FrameA.Weights.Add(TEXT("jawOpen"), 0.0f);
    FrameB.Weights.Add(TEXT("jawOpen"), 1.0f);

    Buf.Push(FrameA);
    Buf.Push(FrameB);

    TestTrue(TEXT("Buffer has frames after Push"), Buf.HasFrames());
    TestEqual(TEXT("Num() == 2 after two pushes"), Buf.Num(), 2);

    // Interpolate at t=0.5 — expect jawOpen = 0.5 (midpoint).
    // GetInterpolatedWeights -> GetInterpolatedFrame().Weights (template generalization).
    TMap<FString, float> Weights = Buf.GetInterpolatedFrame(0.5f).Weights;
    TestTrue(TEXT("Weights map is non-empty"), Weights.Num() > 0);

    const float* JawOpen = Weights.Find(TEXT("jawOpen"));
    if (TestNotNull(TEXT("jawOpen key exists in interpolated weights"), JawOpen))
    {
        TestTrue(TEXT("jawOpen at t=0.5 is ~0.5"),
            AnimFrameBufferTestHelpers::NearlyEqual(*JawOpen, 0.5f));
    }

    // At t=0.0 (earliest frame boundary), should return FrameA values (no extrapolation).
    TMap<FString, float> WeightsAtA = Buf.GetInterpolatedFrame(0.0f).Weights;
    const float* JawA = WeightsAtA.Find(TEXT("jawOpen"));
    if (TestNotNull(TEXT("jawOpen exists at t=0.0"), JawA))
    {
        TestTrue(TEXT("jawOpen at t=0.0 is ~0.0"),
            AnimFrameBufferTestHelpers::NearlyEqual(*JawA, 0.0f));
    }

    // At t=1.0 (latest frame boundary), should return FrameB values.
    TMap<FString, float> WeightsAtB = Buf.GetInterpolatedFrame(1.0f).Weights;
    const float* JawB = WeightsAtB.Find(TEXT("jawOpen"));
    if (TestNotNull(TEXT("jawOpen exists at t=1.0"), JawB))
    {
        TestTrue(TEXT("jawOpen at t=1.0 is ~1.0"),
            AnimFrameBufferTestHelpers::NearlyEqual(*JawB, 1.0f));
    }

    // At t=2.0 (beyond latest frame), should clamp to latest (no future extrapolation).
    TMap<FString, float> WeightsBeyond = Buf.GetInterpolatedFrame(2.0f).Weights;
    const float* JawBeyond = WeightsBeyond.Find(TEXT("jawOpen"));
    if (TestNotNull(TEXT("jawOpen exists at t=2.0"), JawBeyond))
    {
        TestTrue(TEXT("jawOpen at t=2.0 clamps to ~1.0"),
            AnimFrameBufferTestHelpers::NearlyEqual(*JawBeyond, 1.0f));
    }

    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 2: Drift calculation — GetDriftMs returns correct sign and magnitude
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryAnimBufDrift,
    "Estuary.Animation.FrameBuffer.DriftCalculation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEstuaryAnimBufDrift::RunTest(const FString& Parameters)
{
    FAnimationFrameBuffer Buf;

    // Empty buffer — drift must be 0.
    TestTrue(TEXT("Drift is 0.0f when buffer is empty"),
        AnimFrameBufferTestHelpers::NearlyEqual(Buf.GetDriftMs(1.0f), 0.0f));

    // Single frame at t=1.0 sec. Audio at t=0.9 sec → animation is 100 ms ahead.
    FEstuaryBotAnimation F1 = AnimFrameBufferTestHelpers::MakeFrame(1.0f);
    Buf.Push(F1);

    const float DriftAhead = Buf.GetDriftMs(0.9f);
    // Positive drift = animation ahead of audio.
    TestTrue(TEXT("Drift ~+100 ms when animation is 100 ms ahead of audio"),
        AnimFrameBufferTestHelpers::NearlyEqual(DriftAhead, 100.0f, 5.0f));

    // Audio at t=1.1 sec → animation is 100 ms behind.
    const float DriftBehind = Buf.GetDriftMs(1.1f);
    // Negative drift = animation behind audio.
    TestTrue(TEXT("Drift ~-100 ms when animation is 100 ms behind audio"),
        AnimFrameBufferTestHelpers::NearlyEqual(DriftBehind, -100.0f, 5.0f));

    // Audio exactly at frame time → zero drift.
    const float DriftZero = Buf.GetDriftMs(1.0f);
    TestTrue(TEXT("Drift ~0 when audio time matches frame exactly"),
        AnimFrameBufferTestHelpers::NearlyEqual(DriftZero, 0.0f, 1.0f));

    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 3: Clear after interrupt — buffer is empty, GetInterpolatedWeights returns empty map
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryAnimBufClearOnInterrupt,
    "Estuary.Animation.FrameBuffer.ClearAfterInterrupt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FEstuaryAnimBufClearOnInterrupt::RunTest(const FString& Parameters)
{
    FAnimationFrameBuffer Buf;

    // Push 10 frames.
    for (int32 i = 0; i < 10; ++i)
    {
        Buf.Push(AnimFrameBufferTestHelpers::MakeFrame(
            static_cast<float>(i) / 30.0f)); // 30 fps timeline
    }

    TestEqual(TEXT("Num() == 10 before Clear"), Buf.Num(), 10);
    TestTrue(TEXT("HasFrames() before Clear"), Buf.HasFrames());

    // Simulate interrupt.
    Buf.Clear();

    TestEqual(TEXT("Num() == 0 after Clear"), Buf.Num(), 0);
    TestFalse(TEXT("HasFrames() == false after Clear"), Buf.HasFrames());

    // GetInterpolatedFrame on empty buffer must return default-constructed (empty) frame (not crash).
    TMap<FString, float> Weights = Buf.GetInterpolatedFrame(0.5f).Weights;
    TestTrue(TEXT("GetInterpolatedFrame.Weights returns empty map after Clear"),
        Weights.Num() == 0);

    // GetDriftMs on empty buffer must return 0.
    TestTrue(TEXT("GetDriftMs returns 0.0f after Clear"),
        AnimFrameBufferTestHelpers::NearlyEqual(Buf.GetDriftMs(0.5f), 0.0f));

    // Buffer must accept new pushes after Clear (no corruption).
    Buf.Push(AnimFrameBufferTestHelpers::MakeFrame(0.1f));
    TestEqual(TEXT("Num() == 1 after push post-Clear"), Buf.Num(), 1);

    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 4: Capacity cap — never grows beyond kAnimFrameBufferCapacity (64); oldest dropped
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryAnimBufCapacityCap,
    "Estuary.Animation.FrameBuffer.CapacityCap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEstuaryAnimBufCapacityCap::RunTest(const FString& Parameters)
{
    FAnimationFrameBuffer Buf;

    // Push exactly kAnimFrameBufferCapacity frames (64).
    for (int32 i = 0; i < kAnimFrameBufferCapacity; ++i)
    {
        Buf.Push(AnimFrameBufferTestHelpers::MakeFrame(
            static_cast<float>(i) / 30.0f));
    }
    TestEqual(TEXT("Num() == 64 at capacity"), Buf.Num(), kAnimFrameBufferCapacity);

    // Push one more frame — oldest must be evicted, count stays at 64.
    const float ExtraTime = static_cast<float>(kAnimFrameBufferCapacity) / 30.0f;
    Buf.Push(AnimFrameBufferTestHelpers::MakeFrame(ExtraTime));
    TestEqual(TEXT("Num() still == 64 after over-capacity push"),
        Buf.Num(), kAnimFrameBufferCapacity);

    // The new frame (latest) must be retrievable — GetInterpolatedFrame at ExtraTime
    // should return a frame with non-empty weights.
    TMap<FString, float> AtLatest = Buf.GetInterpolatedFrame(ExtraTime).Weights;
    TestTrue(TEXT("Latest frame accessible after capacity eviction"),
        AtLatest.Num() > 0);

    // The oldest frame (t=0.0) must have been evicted — GetInterpolatedFrame at t=-1.0
    // (before first remaining frame) should clamp to the earliest remaining frame
    // (which is no longer t=0.0 but t=1/30.0 = ~0.033 sec).
    // We verify that the buffer's earliest stored time is NOT 0.0 by checking that
    // querying at t=0.0 returns the oldest remaining frame, which was pushed at i=1.
    // (No direct API to read raw timestamps — we verify via drift at t=0.0 being > 0.)
    const float DriftAtOrigin = Buf.GetDriftMs(0.0f);
    // Animation is ahead of t=0.0 by at least 1 frame's worth of time (~33 ms at 30 fps).
    TestTrue(TEXT("Drift at t=0.0 reflects oldest eviction (animation ahead >= 30 ms)"),
        DriftAtOrigin >= 30.0f);

    // Push 100 more frames — buffer must never exceed 64.
    for (int32 i = 0; i < 100; ++i)
    {
        Buf.Push(AnimFrameBufferTestHelpers::MakeFrame(
            ExtraTime + static_cast<float>(i + 1) / 30.0f));
    }
    TestTrue(TEXT("Num() <= kAnimFrameBufferCapacity after 100 extra pushes"),
        Buf.Num() <= kAnimFrameBufferCapacity);

    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
