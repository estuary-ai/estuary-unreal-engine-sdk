// EstuaryAnimationFrameBuffer.h — Sorted animation frame ring buffer (templated).
// Plain C++ template (not UObject). Thread safety: FCriticalSection guards all
// public methods. In practice Push() and GetInterpolatedFrame() are both called
// on the game thread (Push from CharacterComponent OnBotAnimation / OnBotPose,
// Get from AnimationReceiver TickComponent / BodyAnimationReceiver TickComponent).
// The section guards for correctness in case a future refactor changes that.
//
// Pitfall 10: GetInterpolatedFrame linearly interpolates between frames so
// 30 Hz frames don't judder at 60+ fps render rates.
// Pitfall 18: Caller provides AudioTimeSec (audio clock) not wall clock.
// Pitfall 23: #pragma once, IWYU only, no CoreMinimal.h.
//
// UE-01 generalization: converted from a hardcoded
// FAnimationFrameBuffer (face TMap<FString,float>) to a template
// TEstuaryAnimationFrameBuffer<FrameT> so the body receiver can reuse it for
// FEstuaryBotPose frames. Existing face call site preserved via
// `using FAnimationFrameBuffer = TEstuaryAnimationFrameBuffer<FEstuaryBotAnimation>;`
// + an inline GetInterpolatedWeights wrapper that delegates to GetInterpolatedFrame().
// All face semantics are preserved byte-for-byte (face spec uses FMath::Lerp on
// per-key Weights map; body spec uses FQuat::Slerp per bone + FVector::Lerp for hips).

#pragma once

#include "CoreTypes.h"
#include "Misc/ScopeLock.h"
#include "HAL/CriticalSection.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Math/UnrealMathUtility.h"
#include "Math/Quat.h"
#include "Math/Vector.h"
#include "Math/Vector4.h"
#include "EstuaryTypes.h"

// Maximum number of frames stored before oldest frames are evicted.
// A2F NIM streams blendshapes at 73-89 FPS in practice (capped at 90 FPS by
// the worker rate-limiter). The original 64-frame cap was sized for the
// SDK_CONTRACT-documented 30 FPS — but A2F outpaces that ~3x, so 64 frames
// covered only ~870ms of an utterance. Once audio playback advanced past
// that window, GetInterpolatedFrame clamped to the latest stored frame
// and animation froze on a stale tail while audio kept moving (visible as
// "lipsync drifting backward then sticking" + drift > 1 s in the receiver
// telemetry). 1024 frames at 90 FPS = ~11 seconds, comfortably longer than
// typical agent utterances. Memory: 1024 * (52 floats + map overhead) ≈
// 300 KB per receiver — negligible. Sort-on-insert remains O(n) but n is
// small per push and cache-friendly.
//
// Body buffer at 30 fps = 1024 / 30 ≈ 34 seconds — same constant works.
static constexpr int32 kAnimFrameBufferCapacity = 1024;

/**
 * Generic audio-clock-anchored frame buffer.
 *
 * Sorted by FrameT::TimeCodeSec; binary-search at interpolation time.
 *
 * Specializations live in EstuaryAnimationFrameBuffer.cpp:
 *   - FEstuaryBotAnimation (face — TMap<FString,float> blendshape weights)
 *   - FEstuaryBotPose      (body — TArray<FEstuaryBotPoseBone> + hips translations)
 *
 * Frame requirements (duck-typed via specialization):
 *   - FrameT must expose `float TimeCodeSec` member
 *   - FrameT must have `bool IsTerminator() const` returning true on terminator frames
 *
 * The generic Push / Clear / HasFrames / Num / GetDriftMs methods work on any
 * FrameT meeting the above duck-type requirements. GetInterpolatedFrame is
 * specialized per FrameT in the .cpp because the per-field interpolation logic
 * differs (face Lerps map values; body Slerps quats + Lerps hips).
 */
template<typename FrameT>
struct ESTUARY_API TEstuaryAnimationFrameBuffer
{
    TEstuaryAnimationFrameBuffer() = default;

    // Not copyable — owns the critical section.
    TEstuaryAnimationFrameBuffer(const TEstuaryAnimationFrameBuffer&) = delete;
    TEstuaryAnimationFrameBuffer& operator=(const TEstuaryAnimationFrameBuffer&) = delete;

    /**
     * Push a renderable frame. Silently ignores terminator frames (IsTerminator()).
     * If capacity is full, the oldest frame is evicted before inserting.
     * Face spec: weights are clamped to [0.0, 1.0] on insertion (T-06-01-01 mitigation).
     * Body spec: quaternions are pushed verbatim (Slerp normalizes during interpolation).
     * Thread-safe.
     */
    void Push(const FrameT& Frame);

    /**
     * Returns linearly/spherically interpolated frame data for the given audio-clock time.
     *
     * Interpolation:
     *   - If AudioTimeSec <= earliest frame, returns earliest frame (no extrapolation).
     *   - If AudioTimeSec >= latest frame, returns latest frame.
     *   - Otherwise, finds surrounding PrevFrame / NextFrame and interpolates per-field.
     *     Alpha = (AudioTimeSec - PrevFrame.TimeCodeSec) /
     *             (NextFrame.TimeCodeSec - PrevFrame.TimeCodeSec)
     *
     * Face spec: per-key FMath::Lerp on Weights map.
     * Body spec: per-bone FQuat::Slerp on rotations + FVector::Lerp on hips translations.
     *
     * Returns default-constructed FrameT when the buffer has no frames.
     * Thread-safe.
     */
    FrameT GetInterpolatedFrame(float AudioTimeSec) const;

    /**
     * Returns the drift in milliseconds between AudioTimeSec and the nearest stored frame.
     * Positive = animation is ahead of audio; Negative = animation is behind.
     * Returns 0.0f when buffer is empty.
     * Thread-safe.
     */
    float GetDriftMs(float AudioTimeSec) const;

    /**
     * Clear all frames. Called on interrupt or message_id transition.
     * Thread-safe.
     */
    void Clear();

    /** Returns true when the buffer contains at least one frame. Thread-safe. */
    bool HasFrames() const;

    /** Returns the number of stored frames (diagnostic). Thread-safe. */
    int32 Num() const;

    /**
     * Returns the TimeCodeSec of the latest (most recently produced) frame,
     * or 0.0f when the buffer is empty. Used by wall-clock-fallback callers
     * to resync their anchor so the audio clock tracks the buffer's
     * production rate instead of drifting unboundedly ahead.
     * Thread-safe.
     */
    float GetLatestFrameTime() const;

private:
    mutable FCriticalSection Section;

    // Frames sorted ascending by TimeCodeSec. TArray is fine here — we maintain
    // sort order on Push and the capacity cap keeps it small.
    TArray<FrameT> Frames;
};

// ---------------------------------------------------------------------------
// Backward-compat alias for face callsite. FAnimationFrameBuffer was the
// pre-Plan-04-04 name; the using-alias keeps EstuaryAnimationReceiver.cpp byte-
// identical at the type-name level. New code (body receiver) instantiates the
// template directly via TEstuaryAnimationFrameBuffer<FEstuaryBotPose>.
// ---------------------------------------------------------------------------
using FAnimationFrameBuffer = TEstuaryAnimationFrameBuffer<FEstuaryBotAnimation>;

/**
 * Convenience wrapper for face callsite — returns just the Weights map of the
 * interpolated frame. Existing face receiver code calls
 * `FrameBuffer.GetInterpolatedWeights(t)`; this inline wrapper preserves that
 * call site without forcing a `.GetInterpolatedFrame(t).Weights` rewrite.
 *
 * (Inline so the template instantiation is visible at the call site; the
 * underlying GetInterpolatedFrame specialization is in the .cpp.)
 */
inline TMap<FString, float> GetInterpolatedWeights(
    const FAnimationFrameBuffer& Buffer, float AudioTimeSec)
{
    return Buffer.GetInterpolatedFrame(AudioTimeSec).Weights;
}
