// EstuaryCharacter.cpp — AEstuaryCharacter actor implementation.
// Pitfall 23: IWYU includes. No CoreMinimal.h. No Engine.h.

#include "EstuaryCharacter.h"
#include "EstuaryCharacterComponent.h"
#include "EstuaryMicrophoneComponent.h"
#include "EstuaryAnimationReceiver.h"
#include "EstuarySubsystem.h"
#include "EstuaryUserPositionComponent.h"
#include "EstuaryLog.h"
#include "Misc/Guid.h"
#include "Engine/GameInstance.h"

AEstuaryCharacter::AEstuaryCharacter()
{
    // CharacterId defaults to empty — BeginPlay validates and generates if needed.
    // Designers are expected to set this in Blueprint defaults or at spawn time.

    // Create the conversation component as a default sub-object so it appears in
    // the editor's component list automatically. No need to call SetupAttachment —
    // UActorComponent (not USceneComponent) doesn't attach to a transform hierarchy.
    CharacterComponent = CreateDefaultSubobject<UEstuaryCharacterComponent>(
        TEXT("EstuaryCharacterComponent"));

    // Create the microphone component as a default sub-object (Phase 5).
    // Requires Runtime Audio Importer (Fab) to be installed in the project.
    MicrophoneComponent = CreateDefaultSubobject<UEstuaryMicrophoneComponent>(
        TEXT("MicrophoneComponent"));

    // Create the animation receiver as a default sub-object (Phase 6 — ANIM-01..08).
    // Auto-wires to CharacterComponent and AudioPlayerComponent in its own BeginPlay.
    // Set TargetAnimInstance in Blueprint to the MetaHuman face mesh's AnimInstance.
    AnimationReceiver = CreateDefaultSubobject<UEstuaryAnimationReceiver>(
        TEXT("AnimationReceiver"));

    // Create the user-position emitter (GestureSync dyadic conditioning, paper p_y).
    // Defaults to this actor as the reference frame and the first local player
    // pawn as the user; can be overridden via Blueprint defaults if a custom
    // pawn or reference is desired.
    UserPositionComponent = CreateDefaultSubobject<UEstuaryUserPositionComponent>(
        TEXT("UserPositionComponent"));

    // Actors don't need to tick; all event handling is delegate-driven.
    PrimaryActorTick.bCanEverTick = false;
}

void AEstuaryCharacter::BeginPlay()
{
    // Validate CharacterId BEFORE calling Super so the component (which registers with
    // the subsystem in its own BeginPlay) already sees the correct CharacterId.
    if (CharacterId.IsEmpty())
    {
        // Auto-generate: 32 lowercase hex chars, no dashes (EGuidFormats::Digits).
        // Matches ts-sdk message_id convention (UUID without dashes).
        CharacterId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        UE_LOG(LogEstuary, Warning,
            TEXT("[EstuaryCharacter] CharacterId was empty at BeginPlay; generated '%s'. "
                 "Set CharacterId in Blueprint defaults to control which character this actor connects to."),
            *CharacterId);
    }

    Super::BeginPlay();
}

void AEstuaryCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UGameInstance* GI = GetGameInstance())
    {
        if (UEstuarySubsystem* Sub = GI->GetSubsystem<UEstuarySubsystem>())
        {
            // Multi-character scenes: only the actor that owns the active LK session may
            // tear it down. Without this guard, unloading any character would kick the
            // still-alive characters off LiveKit (regression vs platform_capabilities
            // multi_character: true).
            if (Sub->GetVoiceMode() == EEstuaryVoiceMode::LiveKit &&
                Sub->GetActiveVoiceCharacterId() == CharacterId)
            {
                Sub->LeaveLiveKitRoom();
            }
        }
    }
    Super::EndPlay(EndPlayReason);
}
