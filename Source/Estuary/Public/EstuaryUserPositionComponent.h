// Copyright Estuary AI. All Rights Reserved.
// SDK_CONTRACT §user_pos — emits floor-projected user head xy at the GestureSync
// motion frame rate (~30 Hz) for dyadic body-motion conditioning (paper p_y).

#pragma once

#include "Components/ActorComponent.h"
#include "EstuaryUserPositionComponent.generated.h"

class APawn;

/**
 * Streams the local player's floor-projected head xy to the Estuary backend
 * at the GestureSync motion frame rate. Drop one of these onto the bot character
 * actor (or any actor whose origin/forward defines the agent's reference
 * frame) and the component handles the rest — no Blueprint glue required.
 *
 * The emitted xy is computed as:
 *   1. Take the local player pawn's actor location (or camera, if attached
 *      to a Camera Manager-driven pawn).
 *   2. Translate by -ReferenceActor->Location.
 *   3. Rotate by the inverse of ReferenceActor->Yaw so the agent's forward
 *      is +Y in GestureSync-frame (matches paper §3.1 normalization).
 *   4. Divide by 100 to convert UE cm → meters.
 *
 * The result enters the backend as `{type: "pos", x, y}` on the
 * the backend user-position stream and reaches the model via the
 * `USER_POS` input. When the GestureSync session isn't active, the gateway
 * gracefully buffers the most recent value (LTRIM cap).
 *
 * If ReferenceActor is null, the component falls back to GetOwner(); if a
 * player pawn cannot be resolved (e.g., during early BeginPlay before the
 * local controller has possessed a pawn), the tick is a no-op and retries
 * next tick.
 */
UCLASS(ClassGroup = (Estuary), meta = (BlueprintSpawnableComponent),
       Blueprintable, EditInlineNew, DisplayName = "Estuary User Position")
class ESTUARY_API UEstuaryUserPositionComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UEstuaryUserPositionComponent();

    /** Reference actor whose origin + forward define the agent's local frame.
     *  Leave null to use the component's owning actor. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Estuary",
              meta = (ToolTip = "Actor whose origin and forward direction define the GestureSync agent reference frame. Defaults to GetOwner() when null."))
    TObjectPtr<AActor> ReferenceActor = nullptr;

    /** Optional override for the player whose head position is reported.
     *  When null the component resolves the local player pawn via the world
     *  context. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Estuary",
              meta = (ToolTip = "Pawn whose location is reported as the user head position. Defaults to GetWorld()->GetFirstPlayerController()->GetPawn() when null."))
    TObjectPtr<APawn> UserPawn = nullptr;

    /** Emit rate in Hz. GestureSync motion frame rate is 30 fps; rates much higher
     *  than that waste bandwidth without improving quality. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Estuary",
              meta = (ClampMin = "1.0", ClampMax = "120.0",
                      ToolTip = "Position emit rate in Hz. 30 Hz matches the GestureSync motion frame rate; values above ~60 Hz are pure overhead."))
    float EmitRateHz = 30.0f;

    /** When false, EmitNow() must be called manually (useful when the host
     *  project drives position from a custom blend or smoothing layer). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Estuary",
              meta = (ToolTip = "When true (default) the component auto-emits each tick at EmitRateHz. Disable to drive position manually via EmitNow()."))
    bool bAutoEmit = true;

    /** Force one position emission with the current pawn/reference state. */
    UFUNCTION(BlueprintCallable, Category = "Estuary",
              meta = (ToolTip = "Compute and emit one user_pos sample now. Use when bAutoEmit is false."))
    void EmitNow();

protected:
    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                               FActorComponentTickFunction* ThisTickFunction) override;

private:
    /** Time accumulator for rate throttling (seconds). */
    float TimeSinceLastEmit = 0.0f;

    /** Resolve the user pawn — UserPawn override or first local player pawn. */
    APawn* ResolveUserPawn() const;

    /** Resolve the reference actor — ReferenceActor override or the owner. */
    AActor* ResolveReferenceActor() const;
};
