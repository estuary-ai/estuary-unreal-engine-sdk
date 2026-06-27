// EstuaryUserPosMath.h — pure-math agent-local floor projection for `user_pos`.
// Pitfall 23: #pragma once, IWYU only, no CoreMinimal.h, no Engine.h.
//
// Mirror of EstuaryBodyFrameTransform.h pattern: pure-math, header-only namespace,
// constexpr constants + namespaced inline helpers, no UObject deps — so the logic
// is unit-testable without a World/Actor (EstuaryUserPosMathTests.cpp).
//
// SDK_CONTRACT §user_pos (REQUIRED-FOR-BODY): the GestureSync paper p_y is the user's
// floor-projected head position in METERS, expressed in the agent's reference frame,
// with:
//     wire x = the agent's LEFT  (SMPL-X +x; the web demo passes three.js x through raw)
//     wire y = the agent's FORWARD (SMPL-X +z)
// EMPIRICALLY FALSIFIED 2026-06-09: the prior "+x = right" convention (docs + backend
// docstring) produced left/right MIRRORED gaze in live PIE (user at agent's right ->
// head turned 93 deg LEFT; user at left -> 99 deg RIGHT; gaze_samples.json). The model
// frame is SMPL-X where +x is the agent's anatomical LEFT. UE local space has
// +X = actor forward, +Y = actor right, so wire x = -LocalRight, wire y = LocalForward.
// ProjectToGestureSyncFrame is the single source of truth; the tests pin the convention.

#pragma once

#include "Math/UnrealMathUtility.h"
#include "Math/Vector.h"

namespace EstuaryUserPosMath
{
    /** UE centimeters → GestureSync paper meters. Single source of truth for the scale. */
    constexpr float kUEToMeters = 0.01f;

    /**
     * Project a world-space agent→user delta (UE cm) into the user-position
     * frame (SDK_CONTRACT §user_pos): meters, agent-local.
     *
     * @param WorldDeltaCm  PawnLocation - AgentLocation, in UE world centimeters.
     * @param AgentYawDeg   The agent actor's world yaw in degrees (pitch/roll ignored — floor xy).
     * @param OutX          Output wire x = the agent's LEFT (SMPL-X +x), in meters.
     * @param OutY          Output wire y = the agent's FORWARD, in meters.
     */
    inline void ProjectToGestureSyncFrame(const FVector& WorldDeltaCm, float AgentYawDeg, float& OutX, float& OutY)
    {
        const float YawRad = FMath::DegreesToRadians(AgentYawDeg);
        const float CosYaw = FMath::Cos(YawRad);
        const float SinYaw = FMath::Sin(YawRad);

        // Inverse-yaw rotation of the world delta into the agent's local frame.
        // UE local convention: +X = forward, +Y = right.
        const float LocalForward = CosYaw * WorldDeltaCm.X + SinYaw * WorldDeltaCm.Y; // UE +X
        const float LocalRight   = -SinYaw * WorldDeltaCm.X + CosYaw * WorldDeltaCm.Y; // UE +Y

        // Wire convention: x = LEFT (SMPL-X +x), y = FORWARD. The model frame is
        // SMPL-X (right-handed) — emitting +right here mirrors gaze left/right
        // (falsified live 2026-06-09).
        OutX = -LocalRight * kUEToMeters;
        OutY = LocalForward * kUEToMeters;
    }
}
