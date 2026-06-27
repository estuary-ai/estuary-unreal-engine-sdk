// EstuaryReceiverBlendMath.h — pure crossfade alpha + blend math for amplitude-jaw layer.
// Pitfall 23: #pragma once, IWYU. No UObject. Header-only acceptable; testable.
// Quick task 260428-365 (T3).
//
// State machine (per PD-03 in 260428-365-PLAN.md, mirrors web SDK Option 1):
//   - FirstA2fSec < 0           → α = 0  (no A2F seen this utterance)
//   - LastA2fEndSec < 0         → ramp 0→1 over A2FCrossfadeMs/2 from FirstA2fSec
//   - LastA2fEndSec >= 0        → ramp 1→0 over A2FCrossfadeMs/2 from LastA2fEndSec
//
// Once FirstA2fSec is set, alpha stays at 1 for the rest of the utterance regardless
// of bA2fCovers until A2F ends (last covering frame), then ramps 1→0 over
// A2FCrossfadeMs/2. Mid-utterance gaps that "appear" to be A2F-end will get a quick
// ramp 1→0 then ramp 0→1 again when A2F resumes — acceptable; resulting visual is
// jaw briefly drops to amplitude and pops back, which is at most a 100ms perceptual
// blip and matches "amplitude bridges A2F gaps."

#pragma once

#include "Math/UnrealMathUtility.h"

namespace EstuaryBlend
{
    /**
     * Crossfade alpha between amplitude-only (α=0) and full A2F (α=1).
     *
     * @param NowSec           FPlatformTime::Seconds() at tick time.
     * @param FirstA2fSec      Time when first A2F frame of utterance landed (-1.0 = unset).
     * @param LastA2fEndSec    Time when first tick after A2F coverage ended (-1.0 = still covering).
     * @param A2FCrossfadeMs   Total crossfade window (ramp-in + ramp-out), milliseconds.
     */
    inline float ComputeCrossfadeAlpha(
        double NowSec, double FirstA2fSec, double LastA2fEndSec, float A2FCrossfadeMs)
    {
        if (FirstA2fSec < 0.0)         { return 0.0f; }
        if (A2FCrossfadeMs <= 0.0f)    { return (LastA2fEndSec < 0.0) ? 1.0f : 0.0f; }
        const float HalfXfadeSec = (A2FCrossfadeMs * 0.5f) * 0.001f;
        if (LastA2fEndSec < 0.0)
        {
            // Ramp-in or hold.
            const float Elapsed = static_cast<float>(NowSec - FirstA2fSec);
            return FMath::Clamp(Elapsed / HalfXfadeSec, 0.0f, 1.0f);
        }
        // Ramp-out from 1 → 0.
        const float Elapsed = static_cast<float>(NowSec - LastA2fEndSec);
        return FMath::Clamp(1.0f - Elapsed / HalfXfadeSec, 0.0f, 1.0f);
    }
}
