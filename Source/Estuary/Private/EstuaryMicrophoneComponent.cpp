// EstuaryMicrophoneComponent.cpp — Cross-platform mic capture via Runtime Audio Importer.
//
// Pitfall 1:  All UObject touches on game thread. Capture callback ONLY calls AudioQueue.Enqueue.
// Pitfall 2:  HandleSttResponseForBargeIn is UFUNCTION() — required for AddDynamic.
// Pitfall 3:  TWeakObjectPtr guards for WeakSubsystem and WeakCharComp.
// Pitfall 7:  Zero LiveKit references.
// Pitfall 8:  Uses RAI UCapturableSoundWave — NOT UAudioCaptureComponent (Windows-only).
// Pitfall 17: check(IsInGameThread()) at top of every public method.
// Pitfall 23: IWYU includes. No CoreMinimal.h.
//
// Threading model:
//   Capture thread: only AudioQueue.Enqueue and ResampleToMono16k (pure arithmetic).
//   Game thread:    all UObject, subsystem, Blueprint delegate, and SPSC drain ops.

#include "EstuaryMicrophoneComponent.h"
#include "EstuarySubsystem.h"
#include "EstuaryCharacterComponent.h"
#include "EstuaryLiveKitBridge.h"
#include "EstuaryLog.h"
#include "EstuaryTypes.h"
#include "Engine/GameInstance.h"
#include "Misc/Base64.h"
#include "Math/UnrealMathUtility.h"
#include "HAL/PlatformMemory.h"

// Runtime Audio Importer — UCapturableSoundWave.
// Sibling plugin gated by WITH_ESTUARY_RAI in Estuary.Build.cs.
// When absent, mic capture falls back to UE's built-in Audio::FAudioCapture
// (Windows-only — cross-platform builds still need RAI installed).
#if WITH_ESTUARY_RAI
#include "Sound/CapturableSoundWave.h"
#else
#include "AudioCaptureCore.h"
#endif

// ============================================================================
// FEstuaryAudioCaptureSession — PIMPL holding the UE built-in mic stream.
// Defined in this TU only so AudioCaptureCore.h does not leak into the public
// header. Non-UObject — destroyed by TUniquePtr from the component destructor.
// ============================================================================

class FEstuaryAudioCaptureSession
{
public:
#if !WITH_ESTUARY_RAI
    Audio::FAudioCapture Capture;
#endif
};

// ============================================================================
// Constructor / Destructor
// ============================================================================

UEstuaryMicrophoneComponent::UEstuaryMicrophoneComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    // Tick needed to drain AudioQueue each frame.
    PrimaryComponentTick.bStartWithTickEnabled = true;
}

// ============================================================================
// BeginPlay
// ============================================================================

void UEstuaryMicrophoneComponent::BeginPlay()
{
    Super::BeginPlay();
    check(IsInGameThread());

    // Cache subsystem.
    if (UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
    {
        WeakSubsystem = GI->GetSubsystem<UEstuarySubsystem>();
    }

    // Cache sibling CharacterComponent.
    WeakCharComp = GetOwner()
        ? GetOwner()->FindComponentByClass<UEstuaryCharacterComponent>()
        : nullptr;

    // Bind for auto-barge-in if sibling exists.
    BindSttResponseForBargeIn();

#if WITH_ESTUARY_RAI
    // Pre-create the UCapturableSoundWave so it is ready at StartListening time.
    // NewObject with outer = this prevents premature GC.
    CapturableWave = NewObject<UCapturableSoundWave>(this, TEXT("EstuaryCapturableWave"));
#else
    UE_LOG(LogEstuary, Log,
        TEXT("[EstuaryMicrophoneComponent] BeginPlay — RAI plugin absent; using UE built-in ")
        TEXT("Audio::FAudioCapture fallback (Windows-only)."));
#endif

    UE_LOG(LogEstuary, Log,
        TEXT("[EstuaryMicrophoneComponent] BeginPlay — subsystem=%s charcomp=%s"),
        WeakSubsystem.IsValid() ? TEXT("ok") : TEXT("null"),
        WeakCharComp.IsValid()  ? TEXT("ok") : TEXT("null"));
}

// ============================================================================
// EndPlay
// ============================================================================

void UEstuaryMicrophoneComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    check(IsInGameThread());

    if (bIsCapturing)
    {
        StopListening();
    }

#if WITH_ESTUARY_RAI
    CapturableWave = nullptr;
#endif
    WeakSubsystem.Reset();
    WeakCharComp.Reset();

    Super::EndPlay(EndPlayReason);
}

// ============================================================================
// StartListening
// ============================================================================

void UEstuaryMicrophoneComponent::StartListening()
{
    check(IsInGameThread());

    if (bIsCapturing)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[EstuaryMicrophoneComponent] StartListening: already capturing — no-op"));
        return;
    }

    UEstuarySubsystem* Subsystem = WeakSubsystem.Get();
    if (!Subsystem)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[EstuaryMicrophoneComponent] StartListening: no subsystem — cannot start"));
        return;
    }

#if WITH_ESTUARY_RAI
    // CapturableWave is stored as UObject* (UHT-friendly). Cast to the RAI type for use.
    UCapturableSoundWave* TypedWave = Cast<UCapturableSoundWave>(CapturableWave.Get());
    if (!TypedWave)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[EstuaryMicrophoneComponent] StartListening: CapturableWave is null or wrong type — was BeginPlay called?"));
        return;
    }

    // Get CharacterId from sibling component for subsystem routing.
    FString CharId;
    if (UEstuaryCharacterComponent* CharComp = WeakCharComp.Get())
    {
        CharId = CharComp->GetCharacterId();
    }

    // Set capture callback BEFORE StartCapture so no frames are lost.
    // CAPTURE THREAD: ONLY AudioQueue.Enqueue and ResampleToMono16k allowed here.
    // NO UObject methods, NO UPROPERTY access, NO TArray::Add (use SetNumUninitialized).
    TypedWave->OnAudioChunkReceived =
        [this](const TArray<uint8>& RawPCMBytes, float SampleRate)
        {
            // Raw bytes from RAI are float32 samples, interleaved channels.
            const float* FloatSamples = reinterpret_cast<const float*>(RawPCMBytes.GetData());
            const int32 NumFloatSamples = RawPCMBytes.Num() / static_cast<int32>(sizeof(float));

            // RAI default capture is mono; treat as 1 channel.
            // If hardware delivers stereo, ResampleToMono16k mixes to mono.
            constexpr int32 NumChannels = 1;

            TArray<int16> Mono16k;
            ResampleToMono16k(FloatSamples, NumFloatSamples, NumChannels, SampleRate, Mono16k);

            if (Mono16k.Num() == 0) { return; }

            // Pack int16 samples into byte array for queue.
            TArray<uint8> PCM16Bytes;
            PCM16Bytes.SetNumUninitialized(Mono16k.Num() * static_cast<int32>(sizeof(int16)));
            FMemory::Memcpy(PCM16Bytes.GetData(), Mono16k.GetData(), PCM16Bytes.Num());

            // SPSC Enqueue — only safe operation from this thread.
            AudioQueue.Enqueue(MoveTemp(PCM16Bytes));
        };

    TypedWave->StartCapture(0); // device 0 = default microphone

    // Notify subsystem — emits start_voice to server and sets ActiveVoiceCharacterId.
    Subsystem->StartVoiceSession(CharId);

    bIsCapturing = true;
    UE_LOG(LogEstuary, Log,
        TEXT("[EstuaryMicrophoneComponent] capture started (char='%s')"), *CharId);
#else
    // Built-in mic path (Audio::FAudioCapture). Windows-only per UE; for
    // cross-platform builds install the RuntimeAudioImporter sibling plugin.
    if (!BuiltInCapture.IsValid())
    {
        BuiltInCapture = MakePimpl<FEstuaryAudioCaptureSession>();
    }

    // CharacterId for subsystem routing.
    FString CharId;
    if (UEstuaryCharacterComponent* CharComp = WeakCharComp.Get())
    {
        CharId = CharComp->GetCharacterId();
    }

    // Capture-thread callback: same shape as the RAI path — float32 samples in,
    // resample to mono 16k, enqueue to SPSC. NO UObject access here (Pitfall 1).
    Audio::FOnAudioCaptureFunction OnCapture =
        [this](const void* InAudio, int32 NumFrames, int32 NumChannels,
               int32 SampleRate, double /*StreamTime*/, bool /*bOverflow*/)
        {
            if (!InAudio || NumFrames <= 0 || NumChannels <= 0) { return; }
            const float* FloatSamples = static_cast<const float*>(InAudio);
            const int32  NumFloatSamples = NumFrames * NumChannels;

            TArray<int16> Mono16k;
            ResampleToMono16k(FloatSamples, NumFloatSamples, NumChannels,
                              static_cast<float>(SampleRate), Mono16k);

            if (Mono16k.Num() == 0) { return; }

            TArray<uint8> PCM16Bytes;
            PCM16Bytes.SetNumUninitialized(Mono16k.Num() * static_cast<int32>(sizeof(int16)));
            FMemory::Memcpy(PCM16Bytes.GetData(), Mono16k.GetData(), PCM16Bytes.Num());
            AudioQueue.Enqueue(MoveTemp(PCM16Bytes));
        };

    Audio::FAudioCaptureDeviceParams Params;
    // Note on sample rate: Windows WASAPI rejects arbitrary rates (e.g. 16000) with
    // AUDCLNT_E_UNSUPPORTED_FORMAT, so we let the device pick its native rate
    // (typically 48000) and ResampleToMono16k in the callback converts to 16 kHz
    // mono int16 before publish. The wire format / LK push rate is always 16 kHz —
    // the device's native rate is just an internal capture detail.
    constexpr uint32 NumFramesDesired = 1024;

    if (!BuiltInCapture->Capture.OpenAudioCaptureStream(Params, MoveTemp(OnCapture), NumFramesDesired))
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[EstuaryMicrophoneComponent] OpenAudioCaptureStream failed — no microphone available?"));
        BuiltInCapture.Reset();
        return;
    }

    if (!BuiltInCapture->Capture.StartStream())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[EstuaryMicrophoneComponent] StartStream failed — closing stream"));
        BuiltInCapture->Capture.CloseStream();
        BuiltInCapture.Reset();
        return;
    }

    Subsystem->StartVoiceSession(CharId);
    bIsCapturing = true;

    UE_LOG(LogEstuary, Log,
        TEXT("[EstuaryMicrophoneComponent] capture started via Audio::FAudioCapture ")
        TEXT("(char='%s', deviceSampleRate=%d Hz, publishRate=16000 Hz mono)"),
        *CharId, BuiltInCapture->Capture.GetSampleRate());
#endif
}

// ============================================================================
// StopListening
// ============================================================================

void UEstuaryMicrophoneComponent::StopListening()
{
    check(IsInGameThread());

    if (!bIsCapturing) { return; }

    StopCaptureDevice();

    // Drain any remaining queued audio before emitting stop_voice.
    if (UEstuarySubsystem* Subsystem = WeakSubsystem.Get())
    {
        TArray<uint8> Chunk;
        while (AudioQueue.Dequeue(Chunk))
        {
            if (!bIsMuted && !bIsSuppressed && Chunk.Num() > 0)
            {
                FEstuaryStreamAudio Payload;
                Payload.Audio = FBase64::Encode(Chunk.GetData(), Chunk.Num());
                Subsystem->SendStreamAudio(Payload);
            }
        }

        // Get CharacterId for the stop session call.
        FString CharId;
        if (UEstuaryCharacterComponent* CharComp = WeakCharComp.Get())
        {
            CharId = CharComp->GetCharacterId();
        }
        Subsystem->StopVoiceSession(CharId);
    }

    bIsCapturing = false;
    UE_LOG(LogEstuary, Log, TEXT("[EstuaryMicrophoneComponent] capture stopped"));
}

// ============================================================================
// StopCaptureLocal — local release, NO server emits
// ============================================================================

void UEstuaryMicrophoneComponent::StopCaptureLocal()
{
    check(IsInGameThread());

    if (!bIsCapturing) { return; }

    StopCaptureDevice();

    // Discard queued audio — the server-side voice session is already gone
    // (voice_timeout / session_timeout / disconnect); do NOT stream_audio into
    // a closed STT stream and do NOT emit stop_voice for it.
    TArray<uint8> Discard;
    while (AudioQueue.Dequeue(Discard)) {}

    bIsCapturing = false;
    UE_LOG(LogEstuary, Log,
        TEXT("[EstuaryMicrophoneComponent] capture stopped locally (server already released voice — no stop_voice emitted)"));
}

// ============================================================================
// StopCaptureDevice — shared platform capture teardown
// ============================================================================

void UEstuaryMicrophoneComponent::StopCaptureDevice()
{
    check(IsInGameThread());

#if WITH_ESTUARY_RAI
    // Stop capture and clear callback before any further ticks can enqueue.
    if (UCapturableSoundWave* TypedWave = Cast<UCapturableSoundWave>(CapturableWave.Get()))
    {
        TypedWave->StopCapture();
        TypedWave->OnAudioChunkReceived = nullptr;
    }
#else
    // Built-in path — stop and close the FAudioCapture stream.
    if (BuiltInCapture.IsValid())
    {
        BuiltInCapture->Capture.StopStream();
        BuiltInCapture->Capture.CloseStream();
        BuiltInCapture.Reset();
    }
#endif
}

// ============================================================================
// TickComponent — game-thread SPSC drain
// ============================================================================

void UEstuaryMicrophoneComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    check(IsInGameThread());

    if (!bIsCapturing) { return; }

    UEstuarySubsystem* Subsystem = WeakSubsystem.Get();
    if (!Subsystem) { return; }

    // Phase 8 (LK-03): Forward mute state to LiveKit voice track when active.
    // This ensures the LK mic track respects the user's mute preference.
    // Run once per tick (not per chunk) to avoid redundant SetMuted calls.
    if (Subsystem->GetVoiceMode() == EEstuaryVoiceMode::LiveKit)
    {
        FEstuaryLiveKitBridge& LKBridge = Subsystem->GetLiveKitBridge();
        if (LKBridge.IsAvailable() && LKBridge.GetVoice())
        {
            // Mirror mute state: muted OR suppressed means the LK track should be muted.
            LKBridge.GetVoice()->SetMuted(bIsMuted || bIsSuppressed);
        }
    }

    TArray<uint8> Chunk;
    while (AudioQueue.Dequeue(Chunk))
    {
        // Phase 8 (LK-03): When LiveKit is active, mic audio is published to the LK track
        // via PushMicPcm. Mute/suppress flags still gate emission — silence isn't pushed.
        // Drain the queue even in LK mode to prevent unbounded growth (T-08-12).
        if (Subsystem->GetVoiceMode() == EEstuaryVoiceMode::LiveKit)
        {
            FEstuaryLiveKitBridge& LKBridge = Subsystem->GetLiveKitBridge();
            if (LKBridge.IsAvailable() && LKBridge.GetVoice() && !bIsMuted && !bIsSuppressed && Chunk.Num() > 0)
            {
                const int32 NumSamples = Chunk.Num() / static_cast<int32>(sizeof(int16));
                const int16* Samples   = reinterpret_cast<const int16*>(Chunk.GetData());
                LKBridge.GetVoice()->PushMicPcm(Samples, NumSamples, /*SampleRateHz=*/16000);

                // Per-second diagnostic so we can verify samples are flowing to the LK
                // track. Logs a single line per second with chunk count + sample total.
                static int32 SDiagChunks = 0;
                static int32 SDiagSamples = 0;
                static double SDiagLastSec = 0.0;
                ++SDiagChunks;
                SDiagSamples += NumSamples;
                const double Now = FPlatformTime::Seconds();
                if (Now - SDiagLastSec >= 1.0)
                {
                    UE_LOG(LogEstuary, Log,
                        TEXT("[EstuaryMicrophoneComponent] LK push 1s window: chunks=%d samples=%d (~%dms audio)"),
                        SDiagChunks, SDiagSamples, (SDiagSamples * 1000) / 16000);
                    SDiagChunks = 0;
                    SDiagSamples = 0;
                    SDiagLastSec = Now;
                }
            }
            else if (Chunk.Num() > 0 && (bIsMuted || bIsSuppressed))
            {
                // Drained but dropped — log occasionally so the BP author sees the gating.
                static double SDiagDropLastSec = 0.0;
                const double Now = FPlatformTime::Seconds();
                if (Now - SDiagDropLastSec >= 5.0)
                {
                    UE_LOG(LogEstuary, Verbose,
                        TEXT("[EstuaryMicrophoneComponent] LK chunk dropped (muted=%d suppressed=%d)"),
                        bIsMuted ? 1 : 0, bIsSuppressed ? 1 : 0);
                    SDiagDropLastSec = Now;
                }
            }
            continue;
        }

        if (bIsMuted || bIsSuppressed)
        {
            // Drop chunk — user muted OR auto-suppressed (echo guard). Do NOT emit.
            // Anti-Pattern 5: both flags independently gate emission.
            continue;
        }
        if (Chunk.Num() == 0) { continue; }

        FEstuaryStreamAudio Payload;
        Payload.Audio = FBase64::Encode(Chunk.GetData(), Chunk.Num());
        Subsystem->SendStreamAudio(Payload);
    }
}

// ============================================================================
// SetMuted / SetSuppressed / SetAutoBargeIn
// ============================================================================

void UEstuaryMicrophoneComponent::SetMuted(bool bMuted)
{
    check(IsInGameThread());
    bIsMuted = bMuted;
    // Do NOT touch bIsSuppressed here — they are INDEPENDENT (Anti-Pattern 5).
}

void UEstuaryMicrophoneComponent::SetSuppressed(bool bSuppressed)
{
    check(IsInGameThread());
    bIsSuppressed = bSuppressed;
    // Do NOT touch bIsMuted here — they are INDEPENDENT.
}

void UEstuaryMicrophoneComponent::SetAutoBargeIn(bool bEnable)
{
    check(IsInGameThread());
    bAutoBargeIn = bEnable;
}

// ============================================================================
// BindSttResponseForBargeIn
// ============================================================================

void UEstuaryMicrophoneComponent::BindSttResponseForBargeIn()
{
    check(IsInGameThread());

    UEstuaryCharacterComponent* CharComp = WeakCharComp.Get();
    if (!CharComp) { return; }

    // Pitfall 2: receiver MUST be UFUNCTION. HandleSttResponseForBargeIn is UFUNCTION().
    CharComp->OnSttResponse.AddDynamic(
        this, &UEstuaryMicrophoneComponent::HandleSttResponseForBargeIn);
}

// ============================================================================
// HandleSttResponseForBargeIn (UFUNCTION — required for AddDynamic, Pitfall 2)
// ============================================================================

void UEstuaryMicrophoneComponent::HandleSttResponseForBargeIn(const FEstuarySttResponse& Response)
{
    check(IsInGameThread());

    // Auto-barge-in condition (MIC-06):
    //   - bAutoBargeIn enabled
    //   - Partial result (not is_final) — user spoke mid-bot-TTS
    //   - bIsSuppressed is true (bot is currently speaking; echo guard is active)
    //   - Sibling CharComp exists to issue the Interrupt
    if (!bAutoBargeIn)   { return; }
    if (Response.bIsFinal) { return; }  // only trigger on partials
    if (!bIsSuppressed)  { return; }    // only trigger while bot is speaking

    UEstuaryCharacterComponent* CharComp = WeakCharComp.Get();
    if (!CharComp) { return; }

    UE_LOG(LogEstuary, Log,
        TEXT("[EstuaryMicrophoneComponent] auto barge-in triggered (partial STT while bot speaking)"));

    CharComp->Interrupt();

    // Clear suppression so the mic resumes sending audio now that the bot was interrupted.
    bIsSuppressed = false;
}

// ============================================================================
// ResampleToMono16k — static helper, capture-thread safe
// ============================================================================

// static
void UEstuaryMicrophoneComponent::ResampleToMono16k(
    const float* RawFloatPCM, int32 NumSamples,
    int32 NumChannels, float SourceRate,
    TArray<int16>& Out16kMono)
{
    // Called from CAPTURE THREAD. Only FMath and FMemory used — no UE allocator, no UObject.
    if (NumSamples <= 0 || NumChannels <= 0 || SourceRate <= 0.f)
    {
        Out16kMono.Empty();
        return;
    }

    constexpr float TargetRate = 16000.f;
    const int32 NumInputFrames  = NumSamples / NumChannels;
    const int32 NumOutputFrames = FMath::RoundToInt(
        static_cast<float>(NumInputFrames) * TargetRate / SourceRate);

    if (NumOutputFrames <= 0)
    {
        Out16kMono.Empty();
        return;
    }

    Out16kMono.SetNumUninitialized(NumOutputFrames);

    const float StepRatio = SourceRate / TargetRate;

    for (int32 OutIdx = 0; OutIdx < NumOutputFrames; ++OutIdx)
    {
        const float SrcPos  = static_cast<float>(OutIdx) * StepRatio;
        const int32 SrcFrame = FMath::FloorToInt(SrcPos);
        const float Frac    = SrcPos - static_cast<float>(SrcFrame);

        // Linear interpolation between SrcFrame and SrcFrame+1 — T-05-02-A bound guards.
        const int32 FrameA = FMath::Min(SrcFrame,     NumInputFrames - 1);
        const int32 FrameB = FMath::Min(SrcFrame + 1, NumInputFrames - 1);

        // Mix channels to mono (sum then average).
        float MonoA = 0.f, MonoB = 0.f;
        for (int32 Ch = 0; Ch < NumChannels; ++Ch)
        {
            MonoA += RawFloatPCM[FrameA * NumChannels + Ch];
            MonoB += RawFloatPCM[FrameB * NumChannels + Ch];
        }
        MonoA /= static_cast<float>(NumChannels);
        MonoB /= static_cast<float>(NumChannels);

        const float Interpolated = MonoA + (MonoB - MonoA) * Frac;

        // Clamp to int16 range.
        Out16kMono[OutIdx] = static_cast<int16>(
            FMath::Clamp(Interpolated * 32767.f, -32768.f, 32767.f));
    }
}
