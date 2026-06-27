// EstuaryPlayheadMath.h — pure adaptive-playout slew for the body-animation playhead.
// Pitfall 23: #pragma once, IWYU. No UObject. Header-only; testable
// (Private/Tests/EstuaryPlayheadSlewTests.cpp).
//
// Extracted 2026-06-10 from UEstuaryBodyAnimationReceiver::TickComponent's
// idle-wallclock block (2026-06-09 adaptive-playout slew) so the SAME logic can
// drive both clock modes:
//   - IDLE (wall-clock anchor): worker silence-fill stream between utterances.
//   - AUDIO (audio-clock anchor): real TTS utterances. Speech frames are produced
//     at ~1x real-time (GestureSync inference is paced by the live audio timeline), so a
//     free-running playhead with a small fixed lead starves — leadMs sat at
//     -13..-400 for entire utterances (PIE 2026-06-10) and GetInterpolatedFrame
//     clamped to the newest frame: the pose only changed when a chunk landed,
//     i.e. speech looked like a series of freeze frames while idle was smooth.
//
// The slew keeps the playhead ~kTargetLagSec behind the newest buffered frame:
// each tick the caller free-runs RelT on its clock, then pulls it kSlewAlpha of
// the way toward (LatestFrameT - kTargetLagSec). Steady state with 1x supply
// converges to RelT ≈ target - dt (tight tracking, ~1x playback); if supply
// stalls, RelT asymptotes BELOW LatestFrameT (never starves into the clamp); if
// supply races, playback speeds up mildly over ~0.3 s instead of snapping.
// Divergence beyond kCatastrophicDriftSec (PIE pause, message rollover races,
// missed first-frame anchoring) hard re-anchors to the target.

#pragma once

#include "Math/UnrealMathUtility.h"

namespace EstuaryPlayhead
{
    /** Target playhead lag behind the newest buffered frame. Must exceed one
     *  GestureSync chunk (8 frames / 30 fps = 267 ms) plus delivery jitter, or the
     *  playhead outruns the supply between chunks and clamp-holds on the newest
     *  frame (the freeze-frame failure). Mirrors the web client's adaptive-playout
     *  lead target. */
    inline constexpr float kTargetLagSec = 0.40f;

    /** Pull toward the target-lag point per 1/60s reference tick. Small enough
     *  that corrections read as a mild speed change (~0.3 s convergence), not a
     *  snap. SlewPlayhead exponentiates this for the actual tick length, so the
     *  gain and the supply-stall asymptote (~0.32 s below the newest frame) are
     *  framerate-independent (a fixed per-tick alpha under-pulls below ~48 fps
     *  and lets a stalled-supply playhead cross the newest frame). */
    inline constexpr float kSlewAlpha = 0.05f;

    /** Free-run vs target divergence beyond which we hard re-anchor instead of
     *  slewing (PIE pause, message rollover, missed anchor). */
    inline constexpr float kCatastrophicDriftSec = 2.0f;

    struct FSlewResult
    {
        /** Corrected playhead time (same time-base as the frame TimeCodeSec). */
        float RelT = 0.0f;
        /** True when the catastrophic-drift hard re-anchor fired. */
        bool  bReanchored = false;
    };

    /**
     * One tick of adaptive-playout correction.
     *
     * @param FreeRunRelT   Playhead after free-running this tick's clock advance
     *                      (clock - per-message offset).
     * @param LatestFrameT  TimeCodeSec of the newest buffered frame.
     *                      Caller must ensure the buffer is non-empty.
     * @param DeltaTimeSec  This tick's length (for framerate-independent gain).
     */
    inline FSlewResult SlewPlayhead(float FreeRunRelT, float LatestFrameT, float DeltaTimeSec)
    {
        const float TargetRelT = LatestFrameT - kTargetLagSec;
        if (FMath::Abs(TargetRelT - FreeRunRelT) > kCatastrophicDriftSec)
        {
            return { TargetRelT, true };
        }
        const float Alpha = 1.0f - FMath::Pow(1.0f - kSlewAlpha,
                                              FMath::Max(DeltaTimeSec, 0.0f) * 60.0f);
        return { FMath::Lerp(FreeRunRelT, TargetRelT, Alpha), false };
    }
}
