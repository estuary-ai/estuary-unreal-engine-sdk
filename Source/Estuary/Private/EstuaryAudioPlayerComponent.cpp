// EstuaryAudioPlayerComponent.cpp — Streaming TTS audio player implementation.
//
// Threading model:
//   Game thread  → EnqueueBotVoice, FlushAllForMessage, TickComponent, BeginPlay, EndPlay
//   Audio thread → UEstuarySoundWaveProcedural::OnGeneratePCMAudio (ring buffer drains here)
//
// Pitfall 1:  All UObject touches on game thread — subsystem and delegate dispatch guarantee this.
// Pitfall 2:  HandleBotVoiceEvent and HandleInterruptAckedEvent are UFUNCTION() (see header).
// Pitfall 4:  200 ms pre-buffer gate — IsPrimed() must return true before Play() is called.
// Pitfall 8:  iOS AVAudioSession shim called before first Play() — bIOSSessionActive gate.
// Pitfall 17: check(IsInGameThread()) at the top of every public-facing method.
// Pitfall 23: IWYU includes only. No CoreMinimal.h.

#include "EstuaryAudioPlayerComponent.h"
#include "EstuaryAmplitudeMath.h"
#include "EstuaryCharacterComponent.h"
#include "EstuaryDeveloperSettings.h"
#include "EstuarySubsystem.h"
#include "EstuaryLiveKitBridge.h"
#include "EstuaryLog.h"
#include "Misc/Base64.h"
#include "HAL/Platform.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/AssertionMacros.h"
#include "Engine/GameInstance.h"
#include "Sound/SoundWaveProcedural.h"   // Path B: vanilla wave, not subclass

// iOS audio session shim.
// The actual include and call live inside #if PLATFORM_IOS guards.
#if PLATFORM_IOS
#include "EstuaryIOSAudioSession.h"
#endif

// ============================================================================
// Constructor
// ============================================================================

UEstuaryAudioPlayerComponent::UEstuaryAudioPlayerComponent()
{
    // TickComponent is required for tail-drain detection (audio_playback_complete).
    PrimaryComponentTick.bCanEverTick = true;

    // Path B: vanilla USoundWaveProcedural (NOT a subclass) — sidesteps UE 5.6+
    // IAudioProxyDataFactory::CreateNewProxyData pure-virtual that derived classes hit.
    // Convai pattern: own as a member, feed via QueueAudio(), drain via internal queue.
    //
    // Constructed lazily in OnRegister (not via CreateDefaultSubobject) because
    // USoundWaveProcedural inherits AssetImportData from USoundBase, and a CDO-level
    // subobject forces that AssetImportData into the BP CDO during serialization,
    // which fails with "Illegal reference to private object" because AssetImportData
    // is private to the C++ Default__EstuaryAudioPlayerComponent. Using NewObject
    // with RF_Transient at runtime keeps StreamingSound out of the BP CDO entirely.
}

// ============================================================================
// OnRegister — lazy-construct StreamingSound at runtime
// ============================================================================

void UEstuaryAudioPlayerComponent::OnRegister()
{
    Super::OnRegister();
    if (!StreamingSound)
    {
        StreamingSound = NewObject<USoundWaveProcedural>(
            this, USoundWaveProcedural::StaticClass(),
            TEXT("StreamingSound"), RF_Transient);
        if (StreamingSound)
        {
            StreamingSound->SetSampleRate(CurrentStreamSampleRate);
            StreamingSound->NumChannels = 1;
            SetSound(StreamingSound);
        }
    }
}

// ============================================================================
// BeginPlay
// ============================================================================

void UEstuaryAudioPlayerComponent::BeginPlay()
{
    Super::BeginPlay();
    check(IsInGameThread());

    // Engine envelope follower configuration. Defaults match the receiver's
    // AmplitudeAttackMs/ReleaseMs UPROPERTY defaults (10ms / 100ms — engine uses peak-mode
    // detection, not RMS, so attack is tighter than the web SDK's 30ms RMS).
    // Per-instance receiver overrides will eventually flow here in a future PR; for v1
    // we ship engine defaults that match the locked UPROPERTY defaults.
    EnvelopeFollowerAttackTime  = 10;   // ms (UAudioComponent base UPROPERTY)
    EnvelopeFollowerReleaseTime = 100;  // ms

    // Engine marshals values from audio thread → game thread automatically.
    // .AddUObject (not AddDynamic) because OnAudioSingleEnvelopeValueNative is a
    // multicast non-dynamic native delegate. Receiver is auto-cleaned up on object destruction.
    OnAudioSingleEnvelopeValueNative.AddUObject(
        this, &UEstuaryAudioPlayerComponent::HandleEnvelopeValue);

    // Cache the subsystem for later use in TickComponent (EmitAudioPlaybackComplete).
    if (AActor* Owner = GetOwner())
    {
        if (UGameInstance* GI = Owner->GetGameInstance())
        {
            WeakSubsystem = GI->GetSubsystem<UEstuarySubsystem>();
        }
    }

    // Set the initial stream sample rate on the procedural wave.
    if (StreamingSound)
    {
        StreamingSound->SetSampleRate(CurrentStreamSampleRate);
        StreamingSound->NumChannels = 1;
    }
    TotalBytesQueued = 0;
    ResetAecReference();
    // Session-constant AEC gate (see bAecTapEnabled doc) — read once, not per tick.
    if (const UEstuaryDeveloperSettings* Settings = UEstuaryDeveloperSettings::Get())
    {
        bAecTapEnabled = Settings->bLiveKitEchoCancellation;
    }

    // Auto-wiring: find a sibling UEstuaryCharacterComponent on the same actor and bind.
    // This is the primary wiring path — no Blueprint wiring needed for standard usage.
    // If no sibling is found, the component is still usable via direct EnqueueBotVoice calls.
    AActor* Owner = GetOwner();
    if (!IsValid(Owner))
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[AudioPlayer] BeginPlay: no owning actor — auto-wiring skipped"));
        return;
    }

    UEstuaryCharacterComponent* CharComp =
        Owner->FindComponentByClass<UEstuaryCharacterComponent>();

    if (IsValid(CharComp))
    {
        // Pitfall 2: both receivers are UFUNCTION() — AddDynamic is safe.
        CharComp->OnBotVoice.AddDynamic(
            this, &UEstuaryAudioPlayerComponent::HandleBotVoiceEvent);
        CharComp->OnInterruptAcked.AddDynamic(
            this, &UEstuaryAudioPlayerComponent::HandleInterruptAckedEvent);

        UE_LOG(LogEstuary, Log,
            TEXT("[AudioPlayer] BeginPlay: auto-wired to UEstuaryCharacterComponent on '%s'"),
            *GetNameSafe(Owner));
    }
    else
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[AudioPlayer] BeginPlay: no sibling UEstuaryCharacterComponent found on '%s'. "
                 "Wire OnBotVoice manually or attach UEstuaryCharacterComponent to the same actor."),
            *GetNameSafe(Owner));
    }
}

// ============================================================================
// TickComponent — tail-drain detection for audio_playback_complete
// ============================================================================

void UEstuaryAudioPlayerComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    // Game thread is guaranteed in TickComponent by UE contract.

    // AEC (Pitfall 27): release whatever the audio thread consumed since last tick to
    // the LiveKit APM's far-end reference stream.
    PumpAecReference();

    // Tail-drain detection — WS path (bIsFinal marker available).
    // After the final chunk has been enqueued and all samples have been consumed by
    // the audio thread, emit audio_playback_complete once.
    if (bFinalChunkReceived && bIsPlaying && StreamingSound &&
        StreamingSound->GetAvailableAudioByteCount() == 0)
    {
        UE_LOG(LogEstuary, Log,
            TEXT("[AudioPlayer] audio tail drained — emitting audio_playback_complete (message_id=%s)"),
            *CurrentMessageId);

        if (UEstuarySubsystem* Subsystem = WeakSubsystem.Get())
        {
            FEstuaryAudioPlaybackComplete Complete;
            Complete.MessageId = CurrentMessageId;
            Subsystem->EmitAudioPlaybackComplete(Complete);
        }

        // Reset playback state for the next message.
        bFinalChunkReceived       = false;
        bIsPlaying                = false;
        CurrentMessageId          = TEXT("");
        LastEnqueuedChunkIndex    = -1;
        LastEnqueueTime           = 0.0;
        LastNonSilentEnvelopeTime = 0.0;
    }

    // Tail-drain detection — LiveKit path (no bIsFinal marker).
    //
    // Two failure modes the WS-only detector above doesn't catch:
    //   (a) LK streams audio CONTINUOUSLY — once the WebRTC track is open, the remote
    //       peer pushes silence frames every 10ms even when the bot isn't speaking.
    //       LastEnqueueTime stays fresh and the procedural wave's buffer is never
    //       empty for long, so a "no enqueue + empty buffer" gate would never trip.
    //   (b) WS underrun >150ms can also leave the buffer empty without bIsFinal.
    //
    // Two complementary detectors cover both:
    //   Method 1 (WS underrun): no enqueue for 150ms AND buffer drained.
    //   Method 2 (LK silence): envelope follower has reported silence (<kSilenceThreshold)
    //                          for 150ms — bot is not speaking even though PCM is flowing.
    //
    // EITHER method tripping flips bIsPlaying false → IsBotAudioActive() returns false →
    // UEstuaryAnimationReceiver triggers the 100ms ramp-to-neutral.
    constexpr double kBotAudioIdleSec = 0.15;
    if (!bFinalChunkReceived && bIsPlaying)
    {
        const double NowSec = FPlatformTime::Seconds();

        const bool bEnqueueQuiet = (LastEnqueueTime > 0.0 &&
                                    NowSec - LastEnqueueTime >= kBotAudioIdleSec &&
                                    StreamingSound &&
                                    StreamingSound->GetAvailableAudioByteCount() == 0);

        const bool bEnvelopeSilent = (LastNonSilentEnvelopeTime > 0.0 &&
                                      NowSec - LastNonSilentEnvelopeTime >= kBotAudioIdleSec);

        if (bEnqueueQuiet || bEnvelopeSilent)
        {
            UE_LOG(LogEstuary, Log,
                TEXT("[AudioPlayer] Bot audio idle (%s) — flipping bIsPlaying=false."),
                bEnqueueQuiet
                    ? TEXT("no PCM enqueued + empty buffer; WS underrun or end-of-stream")
                    : TEXT("envelope silent for 150ms; LK end-of-stream"));
            // Don't emit audio_playback_complete — WS tail-drain has its own emission;
            // LK has no per-message contract for it (would confuse the gateway).
            bIsPlaying                = false;
            LastEnqueueTime           = 0.0;
            LastNonSilentEnvelopeTime = 0.0;
        }
    }
}

// ============================================================================
// EndPlay — unbind delegates to prevent dangling callbacks
// ============================================================================

void UEstuaryAudioPlayerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    check(IsInGameThread());

    AActor* Owner = GetOwner();
    if (IsValid(Owner))
    {
        UEstuaryCharacterComponent* CharComp =
            Owner->FindComponentByClass<UEstuaryCharacterComponent>();

        if (IsValid(CharComp))
        {
            CharComp->OnBotVoice.RemoveDynamic(
                this, &UEstuaryAudioPlayerComponent::HandleBotVoiceEvent);
            CharComp->OnInterruptAcked.RemoveDynamic(
                this, &UEstuaryAudioPlayerComponent::HandleInterruptAckedEvent);
        }
    }

    Super::EndPlay(EndPlayReason);
}

// ============================================================================
// EnqueueBotVoice
// ============================================================================

void UEstuaryAudioPlayerComponent::EnqueueBotVoice(const FEstuaryBotVoice& Voice)
{
    check(IsInGameThread());

    // -----------------------------------------------------------------------
    // Phase 8 (LK-03, LK-05): When LiveKit is active, bot audio arrives via the LK
    // audio track (not bot_voice PCM events). Skip PCM enqueue.
    // Still process message_id tracking so interrupts and audio_playback_complete work.
    //
    // SDK_CONTRACT note: The server MAY emit bot_voice with PCM during a LiveKit session.
    // Per the contract (§voice_livekit), LiveKit audio is the authoritative source when
    // LK is active. If bot_voice arrives with PCM in LK mode, discard the PCM but keep
    // message_id tracking for interrupt/playback-complete semantics.
    // -----------------------------------------------------------------------
    {
        UEstuarySubsystem* Subsystem = nullptr;
        if (AActor* Owner = GetOwner())
        {
            if (UGameInstance* GI = Owner->GetGameInstance())
            {
                Subsystem = GI->GetSubsystem<UEstuarySubsystem>();
            }
        }

        if (Subsystem && Subsystem->GetVoiceMode() == EEstuaryVoiceMode::LiveKit)
        {
            // Still track the message for interrupt compatibility.
            if (!Voice.MessageId.IsEmpty() && CurrentMessageId.IsEmpty())
            {
                CurrentMessageId = Voice.MessageId;
            }
            UE_LOG(LogEstuary, Verbose,
                TEXT("[AudioPlayer] Skipping bot_voice PCM enqueue — LiveKit mode active (message_id=%s)."),
                *Voice.MessageId);
            return; // Skip PCM enqueue; LK audio pipeline handles playback.
        }
    }

    // -----------------------------------------------------------------------
    // Message ID filtering: ignore chunks from a different (stale) message turn.
    // T-04-03-01: prevents stale-message audio injection after interrupt.
    // -----------------------------------------------------------------------
    if (!CurrentMessageId.IsEmpty() && Voice.MessageId != CurrentMessageId)
    {
        UE_LOG(LogEstuary, Verbose,
            TEXT("[AudioPlayer] EnqueueBotVoice: chunk message_id='%s' != current='%s' — dropped (stale)"),
            *Voice.MessageId, *CurrentMessageId);
        return;
    }

    // First chunk for a new message — initialise state.
    if (CurrentMessageId.IsEmpty())
    {
        CurrentMessageId       = Voice.MessageId;
        LastEnqueuedChunkIndex = -1;
        bFinalChunkReceived    = false;

        UE_LOG(LogEstuary, Log,
            TEXT("[AudioPlayer] new message started (message_id=%s)"), *CurrentMessageId);
    }

    // -----------------------------------------------------------------------
    // Wire-rate restore + EnableAnimation coercion (AUDIO-07, Pitfall 9).
    //
    // bot_voice PCM is generated at the session's negotiated audio_sample_rate
    // (MakeDefaultAuthPayload sources it from UEstuaryDeveloperSettings::
    // AudioSampleRate; EnableAnimation pins it to 16000 for the A2F gate).
    // CurrentStreamSampleRate can legitimately DIFFER from that: a LiveKit call
    // re-rates the procedural wave to the remote track rate (EnqueueRawPcm16
    // probe, typically 48 kHz), and after the room is released (voice_timeout /
    // leave / transport close) bot audio arrives HERE again — 16 kHz PCM played
    // through a 48 kHz wave is 3x pitch ("squeaky bot" bug, found live
    // 2026-07-09 right after the first voice_timeout release). Mirror the
    // EnqueueRawPcm16 probe in reverse: drop any old-rate tail and reinitialise
    // the wave at the wire rate before enqueuing.
    // (Hand-built auth payloads with a rate different from project settings are
    // not propagated here — pre-existing gap, unchanged by this fix.)
    // -----------------------------------------------------------------------
    int32 WireRate = CurrentStreamSampleRate;
    if (const UEstuaryDeveloperSettings* Settings = UEstuaryDeveloperSettings::Get())
    {
        WireRate = Settings->AudioSampleRate;
    }
    if (bEnableAnimationActive)
    {
        if (WireRate != 16000)
        {
            UE_LOG(LogEstuary, Warning,
                TEXT("[AudioPlayer] EnableAnimation requires 16000 Hz but AudioSampleRate=%d. "
                     "Coercing to 16000. Update UEstuaryDeveloperSettings.AudioSampleRate."),
                WireRate);
        }
        WireRate = 16000;
    }
    if (WireRate > 0 && CurrentStreamSampleRate != WireRate)
    {
        UE_LOG(LogEstuary, Log,
            TEXT("[AudioPlayer] Wave is at %d Hz but bot_voice wire rate is %d Hz — reinitialising procedural wave."),
            CurrentStreamSampleRate, WireRate);
        if (bIsPlaying) { Stop(); bIsPlaying = false; }
        if (StreamingSound)
        {
            StreamingSound->ResetAudio();
            StreamingSound->SetSampleRate(WireRate);
            StreamingSound->NumChannels = 1;
        }
        TotalBytesQueued = 0;
        CurrentStreamSampleRate = WireRate;
        // The AEC mirror may hold bytes recorded at the old rate; PumpAecReference
        // labels pushes with the CURRENT rate, so drop them rather than feed the APM
        // a wrong-rate reference (the baseline resyncs via the empty-FIFO branch).
        ResetAecReference();
    }

    // -----------------------------------------------------------------------
    // Base64 decode → PCM16 int16_t samples.
    // -----------------------------------------------------------------------
    TArray<uint8> PcmBytes;
    if (!FBase64::Decode(Voice.Audio, PcmBytes))
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[AudioPlayer] EnqueueBotVoice: base64 decode failed for chunk_index=%d message_id=%s"),
            Voice.ChunkIndex, *Voice.MessageId);
        return;
    }

    if (PcmBytes.Num() == 0)
    {
        // Empty chunk — can happen on final chunk with no payload; still update state.
        UE_LOG(LogEstuary, Verbose,
            TEXT("[AudioPlayer] EnqueueBotVoice: empty PCM bytes for chunk_index=%d"), Voice.ChunkIndex);
    }
    else if (StreamingSound)
    {
        // Path B: feed the vanilla USoundWaveProcedural's internal queue directly.
        // No more SPSC ring buffer of our own — UE manages that internally.
        // PCM16 sanity: QueueAudio silently rejects odd-sized buffers (engine ensure),
        // which would desync TotalBytesQueued and the AEC mirror from the wave —
        // trim a malformed trailing byte so all three stay consistent.
        const int32 EvenBytes = PcmBytes.Num() & ~1;
        if (EvenBytes > 0)
        {
            StreamingSound->QueueAudio(PcmBytes.GetData(), EvenBytes);
            TotalBytesQueued += EvenBytes;
            AppendAecReference(PcmBytes.GetData(), EvenBytes);
        }
    }

    LastEnqueueTime        = FPlatformTime::Seconds();  // for LK-fallback drain detector
    LastEnqueuedChunkIndex = Voice.ChunkIndex;

    if (Voice.bIsFinal)
    {
        bFinalChunkReceived = true;
        UE_LOG(LogEstuary, Log,
            TEXT("[AudioPlayer] final chunk received (message_id=%s chunk_index=%d)"),
            *Voice.MessageId, Voice.ChunkIndex);
    }

    // -----------------------------------------------------------------------
    // iOS audio session (Pitfall 8): ensure PlayAndRecord before Play().
    // EstuaryIOSAudioSession.h provides EnsureVoiceSession().
    // bIOSSessionActive prevents redundant calls (T-04-04-03 mitigation).
    // -----------------------------------------------------------------------
#if PLATFORM_IOS
    if (!bIOSSessionActive)
    {
        Estuary::IOS::EnsureVoiceSession();
        bIOSSessionActive = true;
    }
#endif

    // -----------------------------------------------------------------------
    // 200 ms pre-buffer gate (Pitfall 4): defer Play() until enough samples
    // are buffered to prevent an underrun pop on startup.
    // -----------------------------------------------------------------------
    // 200 ms gate: PCM16 mono → 2 bytes/sample. PrimeBytes = 2 * SampleRate * 0.2.
    if (!bIsPlaying && StreamingSound)
    {
        const int32 PrimeBytes = (CurrentStreamSampleRate * 2) / 5;
        if (StreamingSound->GetAvailableAudioByteCount() >= PrimeBytes)
        {
            Play();
            bIsPlaying = true;
            UE_LOG(LogEstuary, Log,
                TEXT("[AudioPlayer] Primed (200ms buffered) — starting playback (message_id=%s)"),
                *CurrentMessageId);
        }
    }
}

// ============================================================================
// EnqueueRawPcm16 — LiveKit fast path (no base64 decode)
// ============================================================================

void UEstuaryAudioPlayerComponent::EnqueueRawPcm16(const TArray<uint8>& PcmBytes, int32 RemoteSampleRate)
{
    check(IsInGameThread());
    if (PcmBytes.Num() == 0 || !StreamingSound) { return; }

    // Sample-rate probe: if the bot publishes 48 kHz instead of 16 kHz, reinit the wave.
    // Drop the old-rate bytes and reset the queue counter BEFORE reconfiguring, otherwise
    // any tail samples still in the procedural wave's queue would play back at the new
    // rate (16 kHz tail at 48 kHz = 3x speed) and the audio clock denominator would be
    // wrong for the duration of the stale tail.
    if (RemoteSampleRate > 0 && RemoteSampleRate != CurrentStreamSampleRate)
    {
        UE_LOG(LogEstuary, Log,
            TEXT("[AudioPlayer] Remote sample rate %d differs from current %d — reinitialising procedural wave."),
            RemoteSampleRate, CurrentStreamSampleRate);
        if (bIsPlaying) { Stop(); bIsPlaying = false; }
        StreamingSound->ResetAudio();
        TotalBytesQueued = 0;
        ResetAecReference();
        CurrentStreamSampleRate = RemoteSampleRate;
        StreamingSound->SetSampleRate(RemoteSampleRate);
        StreamingSound->NumChannels = 1;
    }

    StreamingSound->QueueAudio(PcmBytes.GetData(), PcmBytes.Num());
    TotalBytesQueued += PcmBytes.Num();
    AppendAecReference(PcmBytes.GetData(), PcmBytes.Num());
    LastEnqueueTime = FPlatformTime::Seconds();  // for LK-fallback drain detector

#if PLATFORM_IOS
    if (!bIOSSessionActive)
    {
        Estuary::IOS::EnsureVoiceSession();
        bIOSSessionActive = true;
    }
#endif

    // 200ms pre-buffer gate (Pitfall 4) — same as EnqueueBotVoice.
    if (!bIsPlaying && StreamingSound)
    {
        const int32 PrimeBytes = (CurrentStreamSampleRate * 2) / 5;
        if (StreamingSound->GetAvailableAudioByteCount() >= PrimeBytes)
        {
            Play();
            bIsPlaying = true;
            UE_LOG(LogEstuary, Log,
                TEXT("[AudioPlayer] Primed (200ms buffered, LK fast path) — starting playback."));
        }
    }
}

// ============================================================================
// FlushAllForMessage
// ============================================================================

void UEstuaryAudioPlayerComponent::FlushAllForMessage(const FString& MessageId)
{
    check(IsInGameThread());

    // If a specific MessageId was provided, only flush if it matches current.
    if (!MessageId.IsEmpty() && MessageId != CurrentMessageId)
    {
        UE_LOG(LogEstuary, Verbose,
            TEXT("[AudioPlayer] FlushAllForMessage: message_id='%s' != current='%s' — ignored"),
            *MessageId, *CurrentMessageId);
        return;
    }

    UE_LOG(LogEstuary, Log,
        TEXT("[AudioPlayer] FlushAllForMessage: silence-ramp flush (message_id=%s)"),
        *CurrentMessageId);

    // Path B: USoundWaveProcedural::ResetAudio clears its internal queue. We lose the
    // 10ms silence ramp the previous custom subclass had — accept the small click on
    // interrupt as the trade-off for not subclassing (Convai pattern).
    if (StreamingSound)
    {
        StreamingSound->ResetAudio();
    }
    TotalBytesQueued = 0;
    ResetAecReference();

    // Stop UAudioComponent playback.
    if (IsPlaying())
    {
        Stop();
    }

    // Reset state — do NOT emit audio_playback_complete for flushed messages (AUDIO-04).
    // The message was interrupted; the server already knows via client_interrupt.
    bFinalChunkReceived       = false;
    bIsPlaying                = false;
    CurrentMessageId          = TEXT("");
    LastEnqueuedChunkIndex    = -1;
    LastEnqueueTime           = 0.0;  // reset LK-fallback drain timer
    LastNonSilentEnvelopeTime = 0.0;
}

// ============================================================================
// SetEnableAnimation
// ============================================================================

void UEstuaryAudioPlayerComponent::SetEnableAnimation(bool bEnable)
{
    bEnableAnimationActive = bEnable;

    if (bEnable && CurrentStreamSampleRate != 16000)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[AudioPlayer] SetEnableAnimation(true): coercing sample rate "
                 "from %d to 16000 Hz (A2F gate requirement)"),
            CurrentStreamSampleRate);
        CurrentStreamSampleRate = 16000;
        if (StreamingSound)
        {
            StreamingSound->SetSampleRate(16000);
        }
    }
}

// ============================================================================
// UFUNCTION delegate receivers (Pitfall 2 — must be UFUNCTION for AddDynamic)
// ============================================================================

void UEstuaryAudioPlayerComponent::HandleBotVoiceEvent(const FEstuaryBotVoice& Voice)
{
    EnqueueBotVoice(Voice);
}

void UEstuaryAudioPlayerComponent::HandleInterruptAckedEvent(const FEstuaryInterruptPayload& Payload)
{
    FlushAllForMessage(Payload.MessageId);
}

// ============================================================================
// GetPlaybackTimeSeconds (Phase 6 — ANIM-05, Pitfall 18)
// ============================================================================

float UEstuaryAudioPlayerComponent::GetPlaybackTimeSeconds() const
{
    if (!StreamingSound || CurrentStreamSampleRate <= 0)
    {
        return 0.0f;
    }
    // Path B: derive consumed samples from (bytes queued by us) - (bytes still in the wave's queue).
    // The vanilla USoundWaveProcedural exposes GetAvailableAudioByteCount() which is bytes
    // NOT YET CONSUMED by the audio thread; subtracting from our cumulative TotalBytesQueued
    // gives the bytes consumed. /2 because PCM16 = 2 bytes per int16 sample.
    const int64 BytesAvailable = StreamingSound->GetAvailableAudioByteCount();
    const int64 BytesConsumed  = TotalBytesQueued - BytesAvailable;
    if (BytesConsumed <= 0) { return 0.0f; }
    const int64 ConsumedSamples = BytesConsumed / 2;
    return static_cast<float>(ConsumedSamples) / static_cast<float>(CurrentStreamSampleRate);
}

// ============================================================================
// GetCurrentAudioTime — dual-clock hook (Phase 8 — LK-05)
// ============================================================================

float UEstuaryAudioPlayerComponent::GetCurrentAudioTime() const
{
    check(IsInGameThread());

    // Resolve subsystem to check voice mode.
    UEstuarySubsystem* Subsystem = nullptr;
    if (AActor* Owner = GetOwner())
    {
        if (UGameInstance* GI = Owner->GetGameInstance())
        {
            Subsystem = GI->GetSubsystem<UEstuarySubsystem>();
        }
    }

    if (Subsystem && Subsystem->GetVoiceMode() == EEstuaryVoiceMode::LiveKit)
    {
        // LK-05 SHIPPED: read from the LiveKit playout clock (atomic, written by SDK reader thread).
        FEstuaryLiveKitBridge& Bridge = Subsystem->GetLiveKitBridge();
        if (Bridge.IsAvailable() && Bridge.GetVoice())
        {
            return static_cast<float>(Bridge.GetVoice()->GetPlayoutClockSeconds());
        }
        return 0.0f;
    }

    // WebSocket mode: use the procedural sound wave's consumed-sample counter.
    // This is the same path as GetPlaybackTimeSeconds() — the authoritative audio clock
    // for UEstuaryAnimationReceiver (Pitfall 18 / ANIM-05).
    return GetPlaybackTimeSeconds();
}

// ============================================================================
// Amplitude jaw layer — engine envelope follower tap (Quick task 260428-365)
// ============================================================================

void UEstuaryAudioPlayerComponent::HandleEnvelopeValue(
    const UAudioComponent* /*InComponent*/,
    const USoundWave* /*InWave*/,
    const float InValue)
{
    check(IsInGameThread());
    CurrentEnvelopeValue   = InValue;
    const double NowSec = FPlatformTime::Seconds();
    LastEnvelopeUpdateTime = NowSec;
    // Track the last non-silent envelope sample for the LK drain detector.
    // Engine peak-mode envelope at 10/100 attack/release: speech values 0.05-0.3,
    // silence values <0.005. 0.01 threshold is well above noise floor, well below
    // any real bot speech.
    constexpr float kSilenceThreshold = 0.01f;
    if (InValue > kSilenceThreshold)
    {
        LastNonSilentEnvelopeTime = NowSec;
    }
}

float UEstuaryAudioPlayerComponent::GetCurrentJawWeight() const
{
    check(IsInGameThread());
    // Pure math lives in EstuaryAmplitudeMath.h so the same code path is unit-testable
    // without instantiating UObjects. Constants (kJawMax, kAmpCurveExp, kEnvelopeStaleSec)
    // ported from the Estuary web SDK reference
    return EstuaryAmplitude::ComputeJawWeight(
        CurrentEnvelopeValue, FPlatformTime::Seconds(), LastEnvelopeUpdateTime);
}

double UEstuaryAudioPlayerComponent::GetLastEnvelopeUpdateTime() const
{
    check(IsInGameThread());
    return LastEnvelopeUpdateTime;
}

bool UEstuaryAudioPlayerComponent::IsBotAudioActive() const
{
    check(IsInGameThread());
    return bIsPlaying;
}

// ============================================================================
// AEC far-end reference tap (Pitfall 27)
// ============================================================================
//
// The LiveKit APM needs to "hear" what the speakers render so AEC3 can subtract the
// echo from the mic. Enqueue time is a bad reference (200 ms pre-buffer + variable
// ring depth), so we mirror every queued byte and release the mirror at the pace the
// audio thread actually consumes it: per tick, the consumption delta is
//   (TotalBytesQueued - GetAvailableAudioByteCount()) - AecConsumedBaseline.
// Residual error is one game tick (~16 ms) + device output latency, absorbed by the
// AecStreamDelayMs hint and AEC3's internal delay estimator.

bool UEstuaryAudioPlayerComponent::IsAecTapAvailable() const
{
    if (!bAecTapEnabled) { return false; }
    if (UEstuarySubsystem* Subsystem = WeakSubsystem.Get())
    {
        FEstuaryLiveKitBridge& Bridge = Subsystem->GetLiveKitBridge();
        return Bridge.IsAvailable() && Bridge.GetVoice() != nullptr;
    }
    return false;
}

void UEstuaryAudioPlayerComponent::AppendAecReference(const uint8* Data, int32 NumBytes)
{
    if (NumBytes <= 0 || !IsAecTapAvailable()) { return; }
    AecReferenceFifo.Append(Data, NumBytes);
}

void UEstuaryAudioPlayerComponent::PumpAecReference()
{
    if (!StreamingSound) { return; }

    const int64 ConsumedTotal =
        TotalBytesQueued - static_cast<int64>(StreamingSound->GetAvailableAudioByteCount());

    if (AecReferenceFifo.Num() == 0)
    {
        // Nothing mirrored (tap unavailable/disabled, or no audio queued since the last
        // reset). Keep the baseline tracking consumption so the first delta after audio
        // starts flowing again is not inflated by bytes we never mirrored.
        AecConsumedBaseline = ConsumedTotal;
        return;
    }

    int64 DeltaBytes = ConsumedTotal - AecConsumedBaseline;
    if (DeltaBytes <= 0) { return; }
    const int64 UnclampedDelta = DeltaBytes;
    DeltaBytes = FMath::Min<int64>(DeltaBytes, AecReferenceFifo.Num());
    DeltaBytes &= ~static_cast<int64>(1);            // PCM16 alignment (after the clamp)

    if (DeltaBytes > 0)
    {
        if (UEstuarySubsystem* Subsystem = WeakSubsystem.Get())
        {
            FEstuaryLiveKitBridge& Bridge = Subsystem->GetLiveKitBridge();
            if (Bridge.IsAvailable() && Bridge.GetVoice())
            {
                Bridge.GetVoice()->PushPlaybackPcm(
                    reinterpret_cast<const int16*>(AecReferenceFifo.GetData()),
                    static_cast<int32>(DeltaBytes / 2),
                    CurrentStreamSampleRate);
            }
        }
        AecReferenceFifo.RemoveAt(0, static_cast<int32>(DeltaBytes), EAllowShrinking::No);
    }

    // The baseline absorbs the UNclamped delta: USoundWaveProcedural::ResetAudio defers
    // its AvailableByteCount reset to the audio render thread, so ConsumedTotal can be
    // transiently inflated right after a reset. Absorbing the excess here costs at most
    // one mistimed burst instead of permanently pushing the reference ahead of playout.
    AecConsumedBaseline += UnclampedDelta;
}

void UEstuaryAudioPlayerComponent::ResetAecReference()
{
    AecReferenceFifo.Reset();
    AecConsumedBaseline = 0;
}
