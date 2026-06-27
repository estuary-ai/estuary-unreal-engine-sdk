// Copyright Estuary AI. All Rights Reserved.

#include "EstuaryUserPositionComponent.h"

#include "Engine/World.h"
#include "EstuaryLog.h"
#include "EstuarySubsystem.h"
#include "EstuaryUserPosMath.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

UEstuaryUserPositionComponent::UEstuaryUserPositionComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
    // Tick group default is fine — we just need the latest player position
    // before the Socket.IO emit lands. No animation dependency to fight here.
}

void UEstuaryUserPositionComponent::BeginPlay()
{
    Super::BeginPlay();
    // Force first emit on the next tick rather than after a full 1/EmitRateHz
    // window — gives GestureSync a real p_y from frame 1 of the session.
    TimeSinceLastEmit = 1.0f;
}

void UEstuaryUserPositionComponent::TickComponent(
    float DeltaTime,
    ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!bAutoEmit)
    {
        return;
    }
    TimeSinceLastEmit += DeltaTime;
    const float EmitInterval = (EmitRateHz > 0.0f) ? (1.0f / EmitRateHz) : 1.0f;
    if (TimeSinceLastEmit < EmitInterval)
    {
        return;
    }
    TimeSinceLastEmit = 0.0f;
    EmitNow();
}

void UEstuaryUserPositionComponent::EmitNow()
{
    AActor* Reference = ResolveReferenceActor();
    APawn* Pawn = ResolveUserPawn();
    if (!Reference || !Pawn)
    {
        // Pawn not yet possessed (early BeginPlay) — silently retry next tick.
        return;
    }

    // World-space delta (UE cm).
    const FVector RefLoc = Reference->GetActorLocation();
    const FVector PawnLoc = Pawn->GetActorLocation();
    const FVector Delta = PawnLoc - RefLoc;

    // Project into the user-position frame (SDK_CONTRACT §user_pos):
    // meters, agent-local, with wire x = the agent's RIGHT and y = the agent's
    // FORWARD. Pure-math single source of truth lives in EstuaryUserPosMath.h and is
    // regression-pinned by EstuaryUserPosMathTests.cpp (the axis order is the exact
    // mapping whose inversion shipped as a gaze bug — audit 2026-06-06).
    float MetersX = 0.0f;
    float MetersY = 0.0f;
    EstuaryUserPosMath::ProjectToGestureSyncFrame(Delta, Reference->GetActorRotation().Yaw, MetersX, MetersY);

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    UGameInstance* GameInstance = World->GetGameInstance();
    if (!GameInstance)
    {
        return;
    }
    UEstuarySubsystem* Subsystem = GameInstance->GetSubsystem<UEstuarySubsystem>();
    if (!Subsystem)
    {
        return;
    }
    Subsystem->EmitUserPos(MetersX, MetersY);
}

APawn* UEstuaryUserPositionComponent::ResolveUserPawn() const
{
    if (UserPawn)
    {
        return UserPawn;
    }
    UWorld* World = GetWorld();
    if (!World)
    {
        return nullptr;
    }
    APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
    return PC ? PC->GetPawn() : nullptr;
}

AActor* UEstuaryUserPositionComponent::ResolveReferenceActor() const
{
    return ReferenceActor ? ReferenceActor.Get() : GetOwner();
}
