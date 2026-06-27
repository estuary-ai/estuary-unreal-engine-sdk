// EstuaryBlueprintLibrary.cpp — Implementation of UEstuaryBlueprintLibrary static helpers.

#include "EstuaryBlueprintLibrary.h"
#include "EstuarySubsystem.h"
#include "EstuaryDeveloperSettings.h"
#include "EstuaryLog.h"
#include "Misc/Guid.h"
#include "Misc/Base64.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

// ---------------------------------------------------------------------------
// NewMessageId
// ---------------------------------------------------------------------------

FString UEstuaryBlueprintLibrary::NewMessageId()
{
    // EGuidFormats::Digits produces 32-char uppercase hex with no dashes.
    // This matches the message_id convention in SDK_CONTRACT.md and the ts-sdk.
    return FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

// ---------------------------------------------------------------------------
// DecodeBase64ToPcm16
// ---------------------------------------------------------------------------

bool UEstuaryBlueprintLibrary::DecodeBase64ToPcm16(const FString& Base64, TArray<int32>& OutSamples)
{
    OutSamples.Reset();

    TArray<uint8> Bytes;
    const bool bDecodeOk = FBase64::Decode(Base64, Bytes);
    if (!bDecodeOk)
    {
        UE_LOG(LogEstuary, Warning, TEXT("DecodeBase64ToPcm16: FBase64::Decode failed — input is not valid base64."));
        return false;
    }

    // T-09-01-02: Validate byte count is even (each PCM16 sample is 2 bytes).
    if (Bytes.Num() % 2 != 0)
    {
        UE_LOG(LogEstuary, Warning,
               TEXT("DecodeBase64ToPcm16: Decoded byte count %d is odd — not valid PCM16 data."),
               Bytes.Num());
        return false;
    }

    const int32 SampleCount = Bytes.Num() / 2;
    OutSamples.SetNumUninitialized(SampleCount);

    // Reinterpret the raw bytes as int16 pairs and widen to int32 for Blueprint compatibility.
    const int16* SamplesPtr = reinterpret_cast<const int16*>(Bytes.GetData());
    for (int32 i = 0; i < SampleCount; ++i)
    {
        OutSamples[i] = static_cast<int32>(SamplesPtr[i]);
    }

    return true;
}

// ---------------------------------------------------------------------------
// EncodePcm16ToBase64
// ---------------------------------------------------------------------------

bool UEstuaryBlueprintLibrary::EncodePcm16ToBase64(const TArray<int32>& Samples, FString& OutBase64)
{
    OutBase64.Reset();

    if (Samples.IsEmpty())
    {
        UE_LOG(LogEstuary, Warning, TEXT("EncodePcm16ToBase64: Input sample array is empty."));
        return false;
    }

    // Narrow int32 values back to int16 for the wire encoding.
    TArray<int16> NarrowSamples;
    NarrowSamples.SetNumUninitialized(Samples.Num());
    for (int32 i = 0; i < Samples.Num(); ++i)
    {
        // Clamp to int16 range to avoid undefined behaviour on overflow.
        NarrowSamples[i] = static_cast<int16>(
            FMath::Clamp(Samples[i], static_cast<int32>(MIN_int16), static_cast<int32>(MAX_int16)));
    }

    const uint8* BytePtr = reinterpret_cast<const uint8*>(NarrowSamples.GetData());
    const int32 ByteCount = NarrowSamples.Num() * 2;

    OutBase64 = FBase64::Encode(BytePtr, ByteCount);
    return !OutBase64.IsEmpty();
}

// ---------------------------------------------------------------------------
// MakeDefaultAuthPayload
// ---------------------------------------------------------------------------

FEstuaryAuthPayload UEstuaryBlueprintLibrary::MakeDefaultAuthPayload(
    FString ApiKey,
    FString CharacterId,
    FString PlayerId,
    bool bEnableAnimation,
    bool bEnableBodyAnimation,
    int32 SampleRate)
{
    FEstuaryAuthPayload Out;

    // T-09-01-01: Never log ApiKey — move it silently into the struct.
    Out.ApiKey               = MoveTemp(ApiKey);
    Out.CharacterId          = MoveTemp(CharacterId);
    Out.PlayerId             = MoveTemp(PlayerId);
    Out.bEnableAnimation     = bEnableAnimation;
    Out.bEnableBodyAnimation = bEnableBodyAnimation;

    // Face (A2F): NVIDIA Audio2Face accepts the negotiated rate (24000 and others),
    // so face animation needs NO coercion — pass the caller's value through.
    // Body (GestureSync): the server gates bot_pose on audio_sample_rate == 16000
    // (the body audio gate accepts 16000 Hz only).
    // Connecting body animation at any other rate yields body.active=false and zero
    // bot_pose frames, so force 16000 when body animation is requested.
    if (bEnableBodyAnimation && SampleRate != 16000)
    {
        UE_LOG(LogEstuary, Log,
            TEXT("[BlueprintLibrary] MakeDefaultAuthPayload: bEnableBodyAnimation=true — "
                 "forcing AudioSampleRate %d -> 16000 (GestureSync HuBERT 16 kHz gate)."),
            SampleRate);
        Out.AudioSampleRate = 16000;
    }
    else
    {
        Out.AudioSampleRate = SampleRate;
    }

    return Out;
}

// ---------------------------------------------------------------------------
// MakeAuthPayloadFromSettings
// ---------------------------------------------------------------------------

FEstuaryAuthPayload UEstuaryBlueprintLibrary::MakeAuthPayloadFromSettings(
    const FString& CharacterId,
    const FString& PlayerId,
    FString& OutServerUrl)
{
    const UEstuaryDeveloperSettings* Settings = UEstuaryDeveloperSettings::Get();

    OutServerUrl = Settings ? Settings->ServerUrl : FString();
    // Resolves AuthToken, falling back to the ESTUARY_API_KEY env var when empty.
    const FString ApiKey = UEstuaryDeveloperSettings::GetEffectiveAuthToken();
    const int32 SampleRate = Settings ? Settings->AudioSampleRate : 24000;

    // Text/voice testbed: never request face or body animation.
    return MakeDefaultAuthPayload(ApiKey, CharacterId, PlayerId,
        /*bEnableAnimation*/ false, /*bEnableBodyAnimation*/ false, SampleRate);
}

// ---------------------------------------------------------------------------
// GetEstuarySubsystem
// ---------------------------------------------------------------------------

UEstuarySubsystem* UEstuaryBlueprintLibrary::GetEstuarySubsystem(const UObject* WorldContextObject)
{
    // T-09-01-03: Null-check WorldContextObject first; return nullptr gracefully.
    if (!WorldContextObject)
    {
        return nullptr;
    }

    UWorld* World = WorldContextObject->GetWorld();
    if (!World)
    {
        return nullptr;
    }

    UGameInstance* GI = World->GetGameInstance();
    if (!GI)
    {
        return nullptr;
    }

    return GI->GetSubsystem<UEstuarySubsystem>();
}

// ---------------------------------------------------------------------------
// PushFaceCurve (DEPRECATED no-op shim)
// ---------------------------------------------------------------------------
//
// The pre-v0.2 face-animation path that PushFaceCurve targeted (UAnimInstance
// UPROPERTY + Modify Curve) was removed when face curves migrated to Live Link.
// This shim only keeps legacy callers (e.g. BPC_EstuaryAutoBlink) compilable so
// they do not block PIE. It logs once per call and otherwise does nothing.
// ---------------------------------------------------------------------------
void UEstuaryBlueprintLibrary::PushFaceCurve(USceneComponent* FaceComponent, FName CurveName, float Value)
{
    static bool bWarnedOnce = false;
    if (!bWarnedOnce)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[EstuaryBlueprintLibrary] PushFaceCurve('%s'=%.3f) is a deprecated no-op since v0.2. "
                 "Migrate to FEstuaryLiveLinkBridge::GetPublisher()->PublishFrame. "
                 "(This warning is logged once.)"),
            *CurveName.ToString(), Value);
        bWarnedOnce = true;
    }
    (void)FaceComponent;
    (void)CurveName;
    (void)Value;
}
