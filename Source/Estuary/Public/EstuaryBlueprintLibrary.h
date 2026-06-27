// EstuaryBlueprintLibrary.h — Cross-cutting Blueprint helpers for the Estuary Unreal SDK.
// Pitfall 23: #pragma once, no CoreMinimal.h, no Engine.h, IWYU-only includes.

#pragma once

#include "CoreTypes.h"
#include "UObject/ObjectMacros.h"
#include "Containers/UnrealString.h"
#include "Containers/Array.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "EstuaryTypes.h"

// Forward-declare the subsystem to avoid including EstuarySubsystem.h in a Public header.
// Per Pitfall 23 / API-01: EstuarySocketIOBridge internals must NEVER appear in Public/.
class UEstuarySubsystem;

#include "EstuaryBlueprintLibrary.generated.h"

/**
 * UEstuaryBlueprintLibrary — Static Blueprint helpers for the Estuary plugin.
 *
 * Available in the Blueprint palette under "Estuary|Helpers". Provides message-ID
 * generation, base64 PCM16 codec, auth payload factory, and subsystem accessor so
 * Blueprint-only consumers can work with the Estuary API without writing any C++.
 *
 * All functions are pure helpers — no state is stored on this class.
 */
UCLASS(meta = (DisplayName = "Estuary Blueprint Library"))
class ESTUARY_API UEstuaryBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:

    /**
     * Generate a unique message correlation ID in 32-character uppercase hex (no dashes).
     *
     * The format matches the message_id convention used throughout the Estuary SDK
     * (FGuid::NewGuid().ToString(EGuidFormats::Digits)). Pass the result as the
     * MessageId field when constructing outbound payloads manually.
     *
     * @return A 32-character uppercase hex GUID string, e.g. "8A4B1C2D3E4F5A6B7C8D9E0F1A2B3C4D".
     */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Estuary|Helpers",
              meta = (ToolTip = "Generate a unique 32-character hex message ID (no dashes) matching the SDK message_id format. Use for manual payload construction."))
    static FString NewMessageId();

    /**
     * Decode a base64 string into a PCM16 (int16) sample array.
     *
     * Validates that the decoded byte count is even (each PCM16 sample is 2 bytes).
     * Returns false if the input is invalid base64 or the byte count is odd.
     *
     * @param Base64      Base64-encoded PCM16 audio data (e.g. from a bot_voice.Audio field).
     * @param OutSamples  Output array of int16 PCM samples. Populated only on success.
     * @return            True if decode succeeded; false if Base64 is invalid or byte-count is odd.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Helpers",
              meta = (ToolTip = "Decode a base64 string into a PCM16 (int16) sample array. Returns false if the data is invalid or the byte count is odd."))
    static bool DecodeBase64ToPcm16(const FString& Base64, TArray<int32>& OutSamples);

    /**
     * Encode a PCM16 (int16) sample array into a base64 string.
     *
     * Used when constructing stream_audio payloads manually from raw PCM data.
     * Returns false if the input sample array is empty.
     *
     * @param Samples    Array of int16 PCM samples to encode. Must not be empty.
     * @param OutBase64  Output base64 string. Populated only on success.
     * @return           True if encode succeeded; false if Samples is empty.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Helpers",
              meta = (ToolTip = "Encode a PCM16 (int16) sample array into a base64 string for stream_audio payloads. Returns false if Samples is empty."))
    static bool EncodePcm16ToBase64(const TArray<int32>& Samples, FString& OutBase64);

    /**
     * Build a fully-populated FEstuaryAuthPayload struct from individual parameters.
     *
     * Face animation (A2F) accepts the negotiated AudioSampleRate as-is — no coercion.
     * Body animation (GestureSync) is gated to 16000 Hz server-side (HuBERT), so when
     * bEnableBodyAnimation is true this helper forces AudioSampleRate to 16000;
     * connecting body animation at any other rate yields body.active=false and zero
     * bot_pose frames. This is the recommended way to construct auth payloads in
     * Blueprint — use the resulting struct as input to the Connect (Latent) node.
     *
     * @param ApiKey               Estuary developer API key (est_...). Never log this value.
     * @param CharacterId          UUID of the character to connect to.
     * @param PlayerId             Opaque player identifier (scopes session and memory).
     * @param bEnableAnimation     Set true to receive bot_animation ARKit-52 lipsync frames (face / A2F).
     * @param bEnableBodyAnimation Set true to receive bot_pose GestureSync body-animation frames.
     *                             Forces AudioSampleRate to 16000 (HuBERT gate).
     * @param SampleRate           Desired TTS audio sample rate in Hz. Default 24000.
     *                             Ignored (forced to 16000) when bEnableBodyAnimation is true.
     * @return                     A populated FEstuaryAuthPayload ready for Connect (Latent).
     */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Estuary|Helpers",
              meta = (ToolTip = "Build an FEstuaryAuthPayload from individual parameters. Forces AudioSampleRate to 16000 when bEnableBodyAnimation=true (GestureSync HuBERT gate); face animation needs no coercion. Never log the ApiKey.",
                      AdvancedDisplay = "SampleRate"))
    static FEstuaryAuthPayload MakeDefaultAuthPayload(
        FString ApiKey,
        FString CharacterId,
        FString PlayerId,
        bool bEnableAnimation = false,
        bool bEnableBodyAnimation = false,
        int32 SampleRate = 24000);

    /**
     * Build an FEstuaryAuthPayload from Project Settings (UEstuaryDeveloperSettings) for the
     * given character + player, and output the configured gateway URL.
     *
     * Reads AuthToken (ApiKey), AudioSampleRate, and ServerUrl from
     * Project Settings -> Plugins -> Estuary. Animation flags are forced false
     * (text/voice only — no face/body animation). This is the one-node convenience the
     * sample/testbed Blueprints use to Connect without Get-Class-Defaults plumbing.
     *
     * @param CharacterId   UUID of the character to connect to (from the AEstuaryCharacter).
     * @param PlayerId      Opaque player identifier (scopes session + memory).
     * @param OutServerUrl  Receives the gateway WebSocket URL from Project Settings.
     * @return              A populated FEstuaryAuthPayload (animation disabled) ready for Connect.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Helpers",
              meta = (ToolTip = "Build an FEstuaryAuthPayload from Project Settings (AuthToken/AudioSampleRate/ServerUrl) for the given character + player. Animation flags forced false. OutServerUrl returns the configured gateway URL. Dev/testbed convenience — avoids Get-Class-Defaults plumbing."))
    static FEstuaryAuthPayload MakeAuthPayloadFromSettings(
        const FString& CharacterId,
        const FString& PlayerId,
        FString& OutServerUrl);

    /**
     * Get the UEstuarySubsystem from a WorldContextObject.
     *
     * Convenience accessor for Blueprint callers — equivalent to
     * GetGameInstance → GetSubsystem<UEstuarySubsystem>(), but auto-wires the
     * WorldContext pin so callers don't need to fetch the game instance manually.
     * Returns nullptr if the context is invalid or the subsystem is not available.
     *
     * @param WorldContextObject  Any UObject in the current world (auto-wired by Blueprint).
     * @return                    The live UEstuarySubsystem, or nullptr if unavailable.
     */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Estuary|Helpers",
              meta = (WorldContext = "WorldContextObject",
                      ToolTip = "Get the UEstuarySubsystem from the current world. Equivalent to GetGameInstance->GetSubsystem<UEstuarySubsystem>(). Returns nullptr if unavailable."))
    static UEstuarySubsystem* GetEstuarySubsystem(const UObject* WorldContextObject);

    /**
     * DEPRECATED stub for legacy callers (e.g. BPC_EstuaryAutoBlink). The pre-v0.2
     * UPROPERTY/Modify Curve face animation path is gone; face curves now flow through
     * FEstuaryLiveLinkBridge::GetPublisher()->PublishFrame. This stub keeps legacy
     * blueprints compilable as a no-op so they don't block PIE; replace call sites
     * with a Live Link single-frame push when there is time.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Helpers|Deprecated",
              meta = (DeprecatedFunction,
                      DeprecationMessage = "PushFaceCurve is a no-op shim post-v0.2 Live Link migration. Use FEstuaryLiveLinkBridge.PublishFrame instead. This shim exists only to keep legacy blueprints (e.g. BPC_EstuaryAutoBlink) compilable.",
                      DisplayName = "Push Face Curve (Deprecated)",
                      ToolTip = "DEPRECATED no-op. The face animation pipeline migrated to Live Link in v0.2. This shim only exists so legacy blueprints still compile."))
    static void PushFaceCurve(USceneComponent* FaceComponent, FName CurveName, float Value);
};
