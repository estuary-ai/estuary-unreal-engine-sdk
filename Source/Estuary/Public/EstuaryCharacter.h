// EstuaryCharacter.h — Blueprintable AActor hosting an Estuary conversation session.
// Pitfall 23: #pragma once, IWYU, no CoreMinimal.h.
// Pitfall 7: Zero LiveKit references.

#pragma once

#include "CoreTypes.h"
#include "UObject/ObjectMacros.h"
#include "GameFramework/Actor.h"
#include "Containers/UnrealString.h"
#include "EstuaryMicrophoneComponent.h"

#include "EstuaryCharacter.generated.h"

class UEstuaryCharacterComponent;
class UEstuaryAnimationReceiver;
class UEstuaryUserPositionComponent;

/**
 * Blueprintable AActor that hosts an Estuary AI conversation character.
 *
 * Drop into a level (or Blueprint subclass) and set CharacterId to the Estuary
 * character UUID you want this actor to converse with. The UEstuaryCharacterComponent
 * sub-object handles all messaging; the actor is a container.
 *
 * Multi-character scenes: place two AEstuaryCharacter actors with different CharacterIds.
 * Each registers independently with UEstuarySubsystem and receives its own OnBotResponse
 * stream (routed by CharacterId). See Source/Estuary/Public/EstuaryCharacterComponent.h
 * for the full Blueprint-callable API.
 *
 * @note If CharacterId is empty at BeginPlay, the actor auto-generates a GUID-based ID
 *       and logs a Warning. Best practice: set it in the Blueprint default or at spawn.
 */
UCLASS(Blueprintable, BlueprintType,
       meta = (DisplayName = "Estuary Character",
               ToolTip = "Drop into a level to host an Estuary AI conversation. Set CharacterId to the Estuary character UUID."))
class ESTUARY_API AEstuaryCharacter : public AActor
{
    GENERATED_BODY()

public:
    AEstuaryCharacter();

    // -----------------------------------------------------------------------
    // Properties
    // -----------------------------------------------------------------------

    /**
     * Estuary character UUID. Must match the character_id used in the Connect() auth payload
     * for this actor to receive bot_response routing from UEstuarySubsystem.
     *
     * If left empty at BeginPlay, a random GUID is generated and a Warning is logged.
     * For production use, always set this in the Blueprint defaults or at spawn time.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Estuary",
              meta = (DisplayName = "Character ID",
                      ToolTip = "Estuary character UUID. Must match the character_id in the Connect auth payload."))
    FString CharacterId;

    /**
     * The conversation component. All Blueprint-callable text-chat ops are on this component.
     * Exposed as VisibleAnywhere so it appears in the Actor's component list in the editor.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Estuary",
              meta = (DisplayName = "Character Component",
                      ToolTip = "Conversation component. Use its SendText, Interrupt, UpdatePreferences nodes."))
    TObjectPtr<UEstuaryCharacterComponent> CharacterComponent;

    /**
     * Default microphone capture component. Provides cross-platform voice input via
     * Runtime Audio Importer. Configure StartListening / StopListening from Blueprint or
     * call them in response to a push-to-talk input action.
     *
     * BlueprintReadOnly: consumers can read it to call methods; replace with a custom
     * subclass by overriding this default subobject in a Blueprint child class.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary",
              meta = (DisplayName = "Microphone Component",
                      ToolTip = "Default Estuary microphone capture component. Runtime Audio Importer required."))
    TObjectPtr<UEstuaryMicrophoneComponent> MicrophoneComponent;

    /**
     * Animation receiver for ARKit-52 lipsync (Phase 6 — ANIM-01 through ANIM-08).
     *
     * Auto-wires to the sibling UEstuaryCharacterComponent and UEstuaryAudioPlayerComponent
     * in its own BeginPlay. Set TargetAnimInstance in Blueprint to point at the
     * MetaHuman face mesh's UEstuaryFaceAnimInstance.
     *
     * Assign RemapAsset = DT_ArkitRemap_Identity (or a custom variant) to enable
     * curve remapping for MetaHuman Animator rigs.
     *
     * See Docs/METAHUMAN_ABP_GUIDE.md for integration instructions.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Estuary|Animation",
              meta = (DisplayName = "Animation Receiver",
                      ToolTip = "ARKit-52 animation receiver. Set TargetAnimInstance to the MetaHuman face mesh's UEstuaryFaceAnimInstance in Blueprint."))
    TObjectPtr<UEstuaryAnimationReceiver> AnimationReceiver;

    /**
     * Streams floor-projected user head xy to GestureSync at ~30 Hz so the model
     * receives the dyadic conditioning input p_y the paper requires
     * (paper §3: x = G(p_y, a, b)). The component defaults to using this
     * actor as the reference frame and the local player pawn as the user.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Estuary|Animation",
              meta = (DisplayName = "User Position Component",
                      ToolTip = "Auto-emits the local player's floor xy at the GestureSync motion frame rate so GestureSync can condition body motion on the user's spatial position."))
    TObjectPtr<UEstuaryUserPositionComponent> UserPositionComponent;

    // -----------------------------------------------------------------------
    // AActor overrides
    // -----------------------------------------------------------------------

    /** Validates CharacterId (generates a GUID if empty), then calls Super::BeginPlay. */
    virtual void BeginPlay() override;

    /** Belt-and-braces LiveKit leave: subsystem Deinitialize already covers PIE-stop, but
     *  per-actor destroy / level-change paths don't always trigger it. */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
};

/*
 * ============================================================================
 * MULTI-CHARACTER PATTERN (TEXT-06 baseline)
 * ============================================================================
 *
 * How to place two AEstuaryCharacter actors in the same level and verify they
 * receive independent bot_response streams:
 *
 * 1. Add two AEstuaryCharacter actors to your level (e.g., NPC_A and NPC_B).
 *
 * 2. Set CharacterId on each actor in the Blueprint defaults or at spawn:
 *      NPC_A.CharacterId = "character-uuid-A";   // matches UEstuarySubsystem::Connect auth payload character_id
 *      NPC_B.CharacterId = "character-uuid-B";   // different UUID
 *
 * 3. Call UEstuarySubsystem::Connect ONCE with character_id = "character-uuid-A"
 *    (the current authenticated session is scoped to one character_id at a time).
 *
 * 4. Both NPC_A and NPC_B register with the subsystem in their BeginPlay calls.
 *    The registry now holds two entries.
 *
 * 5. When the Level Blueprint calls NPC_A.CharacterComponent.SendText("hello"):
 *      - A bot_response stream arrives for NPC_A (CharacterId="character-uuid-A").
 *      - UEstuarySubsystem routes the chunks ONLY to NPC_A's component.
 *      - NPC_A's OnBotResponse fires for each chunk.
 *      - NPC_B's OnBotResponse does NOT fire — no cross-talk.
 *
 * 6. Verification of independent message_id queues:
 *      - Call NPC_A.CharacterComponent.SendText("first") and immediately call
 *        NPC_B.CharacterComponent.SendText("second") (NPC_B won't get a response
 *        because the session is scoped to NPC_A's character_id, but the component
 *        tracks its own CurrentMessageId without interfering with NPC_A's).
 *      - Both actors' CurrentMessageId are distinct FStrings stored on their own
 *        component instances — independent queues by construction.
 *
 * PHASE 3 SCOPE NOTE:
 *   The server currently responds only to the character_id used in Connect().
 *   For a second character to receive responses, use a second UEstuarySubsystem
 *   (one per GameInstance) or a second Socket.IO connection (future phase).
 *   Phase 3's multi-character requirement is "two actors can coexist without
 *   registry corruption or crashes" — not "two server sessions simultaneously."
 * ============================================================================
 */
