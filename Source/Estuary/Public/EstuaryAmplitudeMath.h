// EstuaryAmplitudeMath.h — pure-math amplitude → jaw weight conversion.
// Pitfall 23: #pragma once, IWYU. No UObject. Header-only acceptable; testable.
// Quick task 260428-365 (T2).
//
// Constants ported from the Estuary web SDK reference (web SDK reference).
// JAW_MAX caps amplitude-driven jaw at 0.6 to leave headroom for A2F to reach 1.0
// when crossfade alpha = 1; AMP_CURVE_EXP applies a 0.7 power curve to compress the
// dynamic range so quiet speech still produces visible mouth motion.

#pragma once

#include "Math/UnrealMathUtility.h"

namespace EstuaryAmplitude
{
    /** Caps amplitude-driven jaw at 0.6 (matches web SDK JawRampLayer JAW_MAX). */
    constexpr float  kJawMax            = 0.6f;

    /** Power curve exponent for perceptual amplitude → jaw mapping
     *  (matches web SDK JawRampLayer AMP_CURVE_EXP). */
    constexpr float  kAmpCurveExp       = 0.7f;

    /** If the engine envelope follower hasn't fired its delegate within this window,
     *  treat the audio as silent (e.g., USoundWaveProcedural underrun, Stop() called,
     *  utterance ended). Engine release time is 100ms by default — we match it here so
     *  the staleness gate triggers AFTER the natural decay, not during it. */
    constexpr double kEnvelopeStaleSec  = 0.1;

    /** Pure function. Maps engine envelope value (0..1) to jaw weight (0..kJawMax)
     *  with a perceptual power curve, gated by staleness check.
     *  Returns 0 if (NowSec - LastUpdateSec) > kEnvelopeStaleSec.
     *  Game thread only (no atomics, no locks). */
    inline float ComputeJawWeight(float Envelope, double NowSec, double LastUpdateSec)
    {
        if (NowSec - LastUpdateSec > kEnvelopeStaleSec) { return 0.0f; }
        if (Envelope <= 0.0f) { return 0.0f; }
        if (Envelope >= 1.0f) { return kJawMax; }
        return FMath::Pow(Envelope, kAmpCurveExp) * kJawMax;
    }
}
