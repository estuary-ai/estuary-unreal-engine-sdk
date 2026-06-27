// EstuaryPlayheadSlewTests.cpp — FAutomationTestBase tests for
// EstuaryPlayhead::SlewPlayhead (adaptive-playout playhead correction).
//
// HOW TO RUN:
//   1. Open your UE 5.6+ project in the editor.
//   2. Window > Session Frontend > Automation tab.
//   3. Filter by "Estuary.Playhead".
//   4. Select tests below and click "Run Tests".
//   Headless: UnrealEditor-Cmd.exe <uproject>
//     -ExecCmds="Automation RunTests Estuary.Playhead; Quit" -unattended -nullrhi
//
// The simulated scenarios mirror the 2026-06-10 PIE logs that root-caused the
// speaking freeze-frames: AUDIO-mode playhead anchored ahead of a ~1x real-time
// supply (relT=0.64 vs latestT=0.50, leadMs -13..-400 all utterance).
// Pitfall 23: IWYU only.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "EstuaryPlayheadMath.h"

namespace
{
    constexpr float kDt = 1.0f / 60.0f;   // 60 fps tick

    // One receiver tick: free-run the playhead by DtSec, then slew.
    float StepPlayhead(float RelT, float LatestFrameT, float DtSec = kDt)
    {
        return EstuaryPlayhead::SlewPlayhead(RelT + DtSec, LatestFrameT, DtSec).RelT;
    }
}

// ---------------------------------------------------------------------------
// Test 1: PIE-observed starvation recovers — playhead starts AHEAD of a 1x
// supply (the speaking freeze-frame state) and must build a positive lead.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryPlayheadStarvationRecovery,
    "Estuary.Playhead.StarvationRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryPlayheadStarvationRecovery::RunTest(const FString& /*Params*/)
{
    // 2026-06-10 PIE log, first AUDIO diag: relT=0.64, latestT=0.50 (lead -140ms).
    float RelT   = 0.64f;
    float Latest = 0.50f;
    const float Lead0 = Latest - RelT;
    for (int32 Tick = 0; Tick < 240; ++Tick)   // 4 s at 60 fps, supply 1x
    {
        Latest += kDt;
        RelT = StepPlayhead(RelT, Latest);
        const float Lead = Latest - RelT;
        TestTrue(TEXT("Lead never degrades below the starting deficit"),
                 Lead >= Lead0 - 0.05f);
    }
    TestTrue(TEXT("Lead recovered positive within 4 s"), Latest - RelT > 0.2f);
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 2: Steady-state tracking — 1x supply converges to ~kTargetLagSec lead
// and ~1x playback speed.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryPlayheadSteadyStateTracking,
    "Estuary.Playhead.SteadyStateTracking",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryPlayheadSteadyStateTracking::RunTest(const FString& /*Params*/)
{
    float RelT   = 0.0f;
    float Latest = 0.4f;
    for (int32 Tick = 0; Tick < 180; ++Tick)   // 3 s warm-up
    {
        Latest += kDt;
        RelT = StepPlayhead(RelT, Latest);
    }
    for (int32 Tick = 0; Tick < 60; ++Tick)    // 1 s measured
    {
        Latest += kDt;
        const float Prev = RelT;
        RelT = StepPlayhead(RelT, Latest);
        const float Lead = Latest - RelT;
        TestTrue(TEXT("Steady-state lead stays in [0.25, 0.50]"),
                 Lead > 0.25f && Lead < 0.50f);
        TestTrue(TEXT("Steady-state playback speed ~1x (per-tick advance within 20%)"),
                 FMath::Abs((RelT - Prev) - kDt) < kDt * 0.2f);
    }
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 3: Bursty chunk supply (8 frames / 267 ms, the GestureSync wire cadence) —
// after convergence the lead never goes negative, so GetInterpolatedFrame
// never clamp-holds on the newest frame (the freeze-frame regression test).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryPlayheadChunkSupplyNeverStarves,
    "Estuary.Playhead.ChunkSupplyNeverStarves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryPlayheadChunkSupplyNeverStarves::RunTest(const FString& /*Params*/)
{
    float RelT   = 0.15f;   // utterance anchor: +latencyComp ahead, buffer just primed
    float Latest = 0.23f;   // first 8-frame chunk
    int32 TicksSinceChunk = 0;
    for (int32 Tick = 0; Tick < 600; ++Tick)   // 10 s at 60 fps
    {
        if (++TicksSinceChunk >= 16)           // ~267 ms chunk cadence
        {
            Latest += 8.0f / 30.0f;
            TicksSinceChunk = 0;
        }
        RelT = StepPlayhead(RelT, Latest);
        if (Tick > 120)                        // post-convergence (2 s)
        {
            TestTrue(TEXT("Post-convergence lead stays positive on chunked supply"),
                     Latest - RelT > 0.0f);
        }
    }
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 4: Supply stall — newest frame frozen; the playhead asymptotes BELOW it
// (slew alone can never push the playhead past the newest frame).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryPlayheadSupplyStall,
    "Estuary.Playhead.SupplyStall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryPlayheadSupplyStall::RunTest(const FString& /*Params*/)
{
    const float Latest = 2.0f;
    float RelT = 1.6f;                          // steady-state lag, then supply stops
    for (int32 Tick = 0; Tick < 600; ++Tick)    // 10 s of stall
    {
        RelT = StepPlayhead(RelT, Latest);
        TestTrue(TEXT("Playhead never crosses the newest frame during a stall"),
                 RelT < Latest);
    }

    // Same stall at 30 fps and 120 fps — the dt-scaled alpha keeps the asymptote
    // below the newest frame at any framerate (a fixed per-tick alpha fails <48fps).
    for (const float Dt : { 1.0f / 30.0f, 1.0f / 120.0f })
    {
        RelT = 1.6f;
        const int32 Ticks = static_cast<int32>(10.0f / Dt);
        for (int32 Tick = 0; Tick < Ticks; ++Tick)
        {
            RelT = StepPlayhead(RelT, Latest, Dt);
            TestTrue(TEXT("Stall asymptote stays below the newest frame at all framerates"),
                     RelT < Latest);
        }
    }
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 5: Catastrophic divergence hard re-anchors (both directions).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryPlayheadCatastrophicReanchor,
    "Estuary.Playhead.CatastrophicReanchor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryPlayheadCatastrophicReanchor::RunTest(const FString& /*Params*/)
{
    // Playhead far AHEAD (message rollover race): snap back to target.
    EstuaryPlayhead::FSlewResult R = EstuaryPlayhead::SlewPlayhead(10.0f, 0.5f, kDt);
    TestTrue(TEXT("Ahead-divergence re-anchors"), R.bReanchored);
    TestTrue(TEXT("Re-anchor lands on target lag"),
             FMath::IsNearlyEqual(R.RelT, 0.5f - EstuaryPlayhead::kTargetLagSec, 1e-4f));

    // Playhead far BEHIND (PIE pause while frames accumulated): snap forward.
    R = EstuaryPlayhead::SlewPlayhead(0.1f, 5.0f, kDt);
    TestTrue(TEXT("Behind-divergence re-anchors"), R.bReanchored);
    TestTrue(TEXT("Re-anchor lands on target lag"),
             FMath::IsNearlyEqual(R.RelT, 5.0f - EstuaryPlayhead::kTargetLagSec, 1e-4f));

    // Inside the window: no re-anchor.
    R = EstuaryPlayhead::SlewPlayhead(1.0f, 1.4f, kDt);
    TestFalse(TEXT("In-window divergence slews, not re-anchors"), R.bReanchored);
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
