// EstuaryAnimationFrameBuffer.cpp — TEstuaryAnimationFrameBuffer<FrameT> implementation.
//
// Pitfall 23: IWYU, no CoreMinimal.h.
// Pitfall 10: Linear/Slerp interpolation between frames prevents 30 Hz judder at 60+ fps.
// T-06-01-01: Face spec clamps weights to [0.0, 1.0] on Push.
//
// UE-01 generalization:
//   - Generic methods (Push, Clear, HasFrames, Num, GetDriftMs) are templated
//     on FrameT and defined inline in the .cpp (visible to both face and body
//     explicit instantiations at the bottom of this file).
//   - GetInterpolatedFrame is specialized per FrameT:
//       * FEstuaryBotAnimation: per-key FMath::Lerp on Weights map (face).
//       * FEstuaryBotPose:      per-bone FQuat::Slerp + FVector::Lerp on hips (body).
//   - Explicit template instantiations at file end ensure ABI symbols are
//     exported under the ESTUARY_API decl.

#include "EstuaryAnimationFrameBuffer.h"
#include "Misc/ScopeLock.h"
#include "Math/UnrealMathUtility.h"
#include "Math/Quat.h"
#include "Math/Vector.h"

// ---------------------------------------------------------------------------
// Generic templated method bodies — work on any FrameT meeting the duck-type
// requirements (TimeCodeSec member, IsTerminator() method).
// ---------------------------------------------------------------------------

template<typename FrameT>
void TEstuaryAnimationFrameBuffer<FrameT>::Push(const FrameT& Frame)
{
    // Terminator frames are not renderable — silently ignore (face & body alike).
    if (Frame.IsTerminator()) return;

    FScopeLock Lock(&Section);

    // Evict oldest frame if at capacity (T-06-02-02: hard cap, no unbounded allocation).
    if (Frames.Num() >= kAnimFrameBufferCapacity)
    {
        Frames.RemoveAt(0);
    }

    // Insert maintaining ascending TimeCodeSec order (linear scan — buffer is small).
    int32 InsertIdx = Frames.Num();
    for (int32 i = Frames.Num() - 1; i >= 0; --i)
    {
        if (Frames[i].TimeCodeSec <= Frame.TimeCodeSec)
        {
            InsertIdx = i + 1;
            break;
        }
        InsertIdx = i;
    }
    Frames.Insert(Frame, InsertIdx);
}

template<typename FrameT>
float TEstuaryAnimationFrameBuffer<FrameT>::GetDriftMs(float AudioTimeSec) const
{
    FScopeLock Lock(&Section);
    if (Frames.Num() == 0) return 0.0f;

    // Find nearest frame TimeCodeSec.
    float Nearest = Frames[0].TimeCodeSec;
    float MinDist = FMath::Abs(AudioTimeSec - Nearest);
    for (const auto& F : Frames)
    {
        float Dist = FMath::Abs(AudioTimeSec - F.TimeCodeSec);
        if (Dist < MinDist) { MinDist = Dist; Nearest = F.TimeCodeSec; }
    }
    // Positive = animation ahead of audio (the nearest frame's timecode is past
    // the audio playhead); negative = animation behind. NOTE: the drift guards in
    // both receivers compare the MAGNITUDE via FMath::Abs(), so this sign is only
    // for telemetry/logging — but it must match the documented semantics above
    // (and the FrameBuffer.DriftCalculation unit test). The previous
    // `(AudioTimeSec - Nearest)` inverted the sign vs. this contract (2026-06-06 fix).
    return (Nearest - AudioTimeSec) * 1000.0f;
}

template<typename FrameT>
void TEstuaryAnimationFrameBuffer<FrameT>::Clear()
{
    FScopeLock Lock(&Section);
    Frames.Empty();
}

template<typename FrameT>
bool TEstuaryAnimationFrameBuffer<FrameT>::HasFrames() const
{
    FScopeLock Lock(&Section);
    return Frames.Num() > 0;
}

template<typename FrameT>
int32 TEstuaryAnimationFrameBuffer<FrameT>::Num() const
{
    FScopeLock Lock(&Section);
    return Frames.Num();
}

template<typename FrameT>
float TEstuaryAnimationFrameBuffer<FrameT>::GetLatestFrameTime() const
{
    FScopeLock Lock(&Section);
    if (Frames.Num() == 0) return 0.0f;
    return Frames.Last().TimeCodeSec;
}

// ---------------------------------------------------------------------------
// FACE specialization: GetInterpolatedFrame for FEstuaryBotAnimation.
// Per-key FMath::Lerp on Weights map. Iterates Prev.Weights as the canonical
// key set so the result has the same key shape as Prev (handles missing keys
// in Next by falling back to Prev's value at that key — preserves face semantics).
//
// Face Push spec also needs to clamp weights to [0,1] before storing
// (T-06-01-01 mitigation). To preserve the original face semantics
// without changing the generic Push, we provide a face-specialized Push
// override below.
// ---------------------------------------------------------------------------

template<>
void TEstuaryAnimationFrameBuffer<FEstuaryBotAnimation>::Push(const FEstuaryBotAnimation& Frame)
{
    if (Frame.IsTerminator()) return;

    // Clamp all weights to [0.0, 1.0] (T-06-01-01 mitigation — face only;
    // body quaternions are not in [0,1] range).
    FEstuaryBotAnimation Clamped = Frame;
    for (auto& Pair : Clamped.Weights)
    {
        Pair.Value = FMath::Clamp(Pair.Value, 0.0f, 1.0f);
    }

    FScopeLock Lock(&Section);

    if (Frames.Num() >= kAnimFrameBufferCapacity)
    {
        Frames.RemoveAt(0);
    }

    int32 InsertIdx = Frames.Num();
    for (int32 i = Frames.Num() - 1; i >= 0; --i)
    {
        if (Frames[i].TimeCodeSec <= Clamped.TimeCodeSec)
        {
            InsertIdx = i + 1;
            break;
        }
        InsertIdx = i;
    }
    Frames.Insert(Clamped, InsertIdx);
}

template<>
FEstuaryBotAnimation
TEstuaryAnimationFrameBuffer<FEstuaryBotAnimation>::GetInterpolatedFrame(float AudioTimeSec) const
{
    FScopeLock Lock(&Section);
    if (Frames.Num() == 0) return FEstuaryBotAnimation{};

    // Clamp to earliest/latest frame (no extrapolation beyond buffer bounds).
    if (AudioTimeSec <= Frames[0].TimeCodeSec)     return Frames[0];
    if (AudioTimeSec >= Frames.Last().TimeCodeSec) return Frames.Last();

    // Binary search for PrevFrame (largest TimeCodeSec <= AudioTimeSec).
    int32 PrevIdx = 0;
    int32 NextIdx = Frames.Num() - 1;
    while (PrevIdx + 1 < NextIdx)
    {
        const int32 Mid = (PrevIdx + NextIdx) / 2;
        if (Frames[Mid].TimeCodeSec <= AudioTimeSec) PrevIdx = Mid;
        else                                         NextIdx = Mid;
    }

    const FEstuaryBotAnimation& Prev = Frames[PrevIdx];
    const FEstuaryBotAnimation& Next = Frames[NextIdx];

    const float Span  = Next.TimeCodeSec - Prev.TimeCodeSec;
    const float Alpha = (Span > KINDA_SMALL_NUMBER)
        ? FMath::Clamp((AudioTimeSec - Prev.TimeCodeSec) / Span, 0.0f, 1.0f)
        : 0.0f;

    FEstuaryBotAnimation Result = Prev;  // copies MessageId, FrameIndex, bIsFinal, etc.
    Result.TimeCodeSec = AudioTimeSec;
    Result.Weights.Reset();
    // Iterate Prev.Weights as the canonical key set (face semantics — preserves
    // existing behavior from the pre-template version).
    for (const auto& Pair : Prev.Weights)
    {
        const float* NextVal = Next.Weights.Find(Pair.Key);
        const float  NextW   = NextVal ? *NextVal : Pair.Value;
        Result.Weights.Add(Pair.Key, FMath::Lerp(Pair.Value, NextW, Alpha));
    }
    return Result;
}

// ---------------------------------------------------------------------------
// BODY specialization: GetInterpolatedFrame for FEstuaryBotPose.
// Per-bone FQuat::Slerp on rotations + per-axis FVector::Lerp on hips
// translations (HipsWorld + HipsLocalToFloor). Iterates the smaller of
// Prev/Next.Bones.Num() defensively (J=52 fixed at the public surface,
// but the buffer guarantees length-flex).
// ---------------------------------------------------------------------------

template<>
FEstuaryBotPose
TEstuaryAnimationFrameBuffer<FEstuaryBotPose>::GetInterpolatedFrame(float AudioTimeSec) const
{
    FScopeLock Lock(&Section);
    if (Frames.Num() == 0) return FEstuaryBotPose{};

    if (AudioTimeSec <= Frames[0].TimeCodeSec)     return Frames[0];
    if (AudioTimeSec >= Frames.Last().TimeCodeSec) return Frames.Last();

    int32 PrevIdx = 0;
    int32 NextIdx = Frames.Num() - 1;
    while (PrevIdx + 1 < NextIdx)
    {
        const int32 Mid = (PrevIdx + NextIdx) / 2;
        if (Frames[Mid].TimeCodeSec <= AudioTimeSec) PrevIdx = Mid;
        else                                         NextIdx = Mid;
    }

    const FEstuaryBotPose& Prev = Frames[PrevIdx];
    const FEstuaryBotPose& Next = Frames[NextIdx];

    const float Span  = Next.TimeCodeSec - Prev.TimeCodeSec;
    const float Alpha = (Span > KINDA_SMALL_NUMBER)
        ? FMath::Clamp((AudioTimeSec - Prev.TimeCodeSec) / Span, 0.0f, 1.0f)
        : 0.0f;

    FEstuaryBotPose Result = Prev;  // copies MessageId, FrameIndex, Fps, EmitEpochMs, bIsFinal.
    Result.TimeCodeSec = AudioTimeSec;
    Result.Bones.Reset(Prev.Bones.Num());

    // GestureSync wire is stable per FrameIndex — index i maps 1:1 between Prev and Next.
    // J=52 fixed at public surface; buffer iterates min length defensively.
    const int32 N = FMath::Min(Prev.Bones.Num(), Next.Bones.Num());
    for (int32 i = 0; i < N; ++i)
    {
        FEstuaryBotPoseBone B = Prev.Bones[i];
        const FQuat PrevQ(Prev.Bones[i].Quat.X, Prev.Bones[i].Quat.Y,
                          Prev.Bones[i].Quat.Z, Prev.Bones[i].Quat.W);
        const FQuat NextQ(Next.Bones[i].Quat.X, Next.Bones[i].Quat.Y,
                          Next.Bones[i].Quat.Z, Next.Bones[i].Quat.W);
        const FQuat Slerped = FQuat::Slerp(PrevQ, NextQ, Alpha);
        B.Quat = FVector4(Slerped.X, Slerped.Y, Slerped.Z, Slerped.W);
        Result.Bones.Add(B);
    }
    Result.HipsWorld        = FMath::Lerp(Prev.HipsWorld,        Next.HipsWorld,        Alpha);
    Result.HipsLocalToFloor = FMath::Lerp(Prev.HipsLocalToFloor, Next.HipsLocalToFloor, Alpha);
    return Result;
}

// ---------------------------------------------------------------------------
// Explicit template instantiations — required because the template definitions
// live in this .cpp (specializations are here, generic methods are here) but
// the ESTUARY_API export decl in the header requires the symbols be emitted
// in this translation unit. Without these, link errors at usage sites in
// EstuaryAnimationReceiver.cpp / EstuaryBodyAnimationReceiver.cpp.
// ---------------------------------------------------------------------------

template struct TEstuaryAnimationFrameBuffer<FEstuaryBotAnimation>;
template struct TEstuaryAnimationFrameBuffer<FEstuaryBotPose>;
