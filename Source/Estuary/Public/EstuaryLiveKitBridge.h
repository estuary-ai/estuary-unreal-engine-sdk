// EstuaryLiveKitBridge.h — IEstuaryLiveKitVoice / IEstuaryLiveKitVideo UINTERFACE pair and
// FEstuaryLiveKitBridge service-locator singleton.
//
// Lives in Source/Estuary/Public/ (core module). ZERO includes from EstuaryLiveKit/ or livekit/.
// The bridge IS the compile-time isolation layer — Pitfall 7.
//
// Phase 8 (LK-01, LK-02, LK-03).
// Pitfall 23: #pragma once, IWYU, no CoreMinimal.h.

#pragma once

#include "UObject/Interface.h"
#include "UObject/ObjectMacros.h"
#include "Containers/UnrealString.h"
#include "Misc/AssertionMacros.h"
#include "HAL/Platform.h"

#include "EstuaryLiveKitBridge.generated.h"

// ---------------------------------------------------------------------------
// EEstuaryVoiceMode — current voice transport mode.
// ---------------------------------------------------------------------------

/**
 * The voice transport mode currently in use.
 *
 * WebSocket — default path. Mic audio streamed via stream_audio events; bot TTS arrives
 *             via bot_voice PCM events.
 * LiveKit   — optional WebRTC path. Mic audio published via a LiveKit audio track;
 *             bot TTS arrives via the remote LiveKit audio track.
 *
 * Inspect via UEstuarySubsystem::GetVoiceMode() (BlueprintPure, Phase 8 LK-03).
 */
UENUM(BlueprintType)
enum class EEstuaryVoiceMode : uint8
{
    WebSocket  UMETA(DisplayName = "WebSocket Voice"),
    LiveKit    UMETA(DisplayName = "LiveKit WebRTC Voice"),
};

// ---------------------------------------------------------------------------
// Non-dynamic multicast delegates — used by IEstuaryLiveKitVoice.
// C++ internal only; not Blueprint-assignable.
// ---------------------------------------------------------------------------

/** Fires on the game thread when the LiveKit room join completes successfully. */
DECLARE_MULTICAST_DELEGATE(FOnLiveKitJoined);

/** Fires on the game thread when the LiveKit room disconnects cleanly. */
DECLARE_MULTICAST_DELEGATE(FOnLiveKitLeft);

/** Fires on the game thread when a LiveKit connection or join error occurs.
 *  Param: ErrorMessage — human-readable description of the failure. */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnLiveKitConnectionError, const FString& /* ErrorMessage */);

// ---------------------------------------------------------------------------
// IEstuaryLiveKitVoice — voice interface (UINTERFACE + interface pair).
// ---------------------------------------------------------------------------

/**
 * UEstuaryLiveKitVoice — UObject half of the UINTERFACE pair.
 * Concrete impl: UEstuaryLiveKitVoiceImpl in the optional EstuaryLiveKit module.
 * MinimalAPI: no generated cpp; NotBlueprintable: C++ module registration only.
 */
UINTERFACE(NotBlueprintable)
class ESTUARY_API UEstuaryLiveKitVoice : public UInterface
{
    GENERATED_BODY()
};

/**
 * IEstuaryLiveKitVoice — interface for optional LiveKit WebRTC voice.
 *
 * All methods MUST be called on the game thread.
 * The concrete implementation (UEstuaryLiveKitVoiceImpl in EstuaryLiveKit module) registers
 * itself via FEstuaryLiveKitBridge::RegisterVoiceImpl on StartupModule and unregisters on
 * ShutdownModule. Core code never includes EstuaryLiveKit headers — it only calls through
 * this interface via FEstuaryLiveKitBridge::GetVoice().
 */
class ESTUARY_API IEstuaryLiveKitVoice
{
    GENERATED_BODY()

public:
    /**
     * Connect to a LiveKit room. Called by the subsystem when livekit_token is received.
     *
     * @param Token  JWT token from the server's livekit_token event.
     *               SECURITY: never log this value — it is a short-lived signed credential.
     * @param Url    LiveKit server URL from the server's livekit_token event.
     * @param Room   Room name from the server's livekit_token event.
     *
     * Thread safety: must be called on the game thread.
     * The implementation broadcasts OnJoined on success or OnConnectionError on failure,
     * both marshaled to the game thread.
     */
    virtual void JoinRoom(const FString& Token, const FString& Url, const FString& Room) = 0;

    /**
     * Disconnect from the current room. Tears down the local audio track and the room object.
     * No-op when not joined.
     * Thread safety: must be called on the game thread.
     * Broadcasts OnLeft synchronously before returning.
     */
    virtual void LeaveRoom() = 0;

    /**
     * Returns true when a room is connected and the local audio track is published.
     * Thread safety: must be called on the game thread.
     */
    virtual bool IsJoined() const = 0;

    /**
     * Set microphone mute state on the local audio track.
     * No-op when not joined. Mirrors Unity SDK SetMuted pattern.
     * Thread safety: must be called on the game thread.
     *
     * @param bMuted  true = mute the local audio track; false = unmute.
     */
    virtual void SetMuted(bool bMuted) = 0;

    /**
     * Push a chunk of PCM16 mono audio into the local LiveKit mic track.
     * Called from UEstuaryMicrophoneComponent::TickComponent (game thread) when
     * GetVoiceMode() == LiveKit.
     *
     * @param Pcm16Samples  Pointer to int16 PCM samples, mono, native-endian.
     * @param NumSamples    Sample count (NOT byte count).
     * @param SampleRateHz  Source sample rate (typically 16000 — must match the
     *                      AudioSource constructed in JoinRoom).
     *
     * No-op when not joined. Game thread only.
     */
    virtual void PushMicPcm(const int16* Pcm16Samples, int32 NumSamples, int32 SampleRateHz) = 0;

    /**
     * Feed speaker/playback PCM into the AEC far-end (reverse) reference stream
     * (Pitfall 27). Called from UEstuaryAudioPlayerComponent::TickComponent at
     * audio-consumption pace: the samples passed here are exactly the bytes the audio
     * render thread consumed from the procedural wave since the previous tick, so the
     * APM's echo reference matches what the speakers are actually rendering. Feeding at
     * enqueue time instead would be wrong by the 200 ms pre-buffer plus the variable
     * ring depth and would prevent AEC3 convergence.
     *
     * No-op when echo cancellation is disabled (UEstuaryDeveloperSettings), when APM
     * creation failed, or when not joined.
     *
     * @param Pcm16Samples  Pointer to int16 PCM samples, mono, native-endian.
     * @param NumSamples    Sample count (NOT byte count).
     * @param SampleRateHz  Playback sample rate. Must be divisible by 100 for exact
     *                      10 ms APM framing (16000 / 24000 / 48000 all qualify).
     *
     * Game thread only.
     */
    virtual void PushPlaybackPcm(const int16* Pcm16Samples, int32 NumSamples, int32 SampleRateHz) = 0;

    /**
     * Returns the current LiveKit playout-clock position in seconds for lipsync sync
     * (LK-05). Sourced from the cumulative remote AudioFrame sample count, written
     * atomically by the SDK reader thread, read by the game thread.
     *
     * Returns 0.0 when not joined or when no remote audio has been received yet.
     * Thread safety: any thread (atomic load).
     */
    virtual double GetPlayoutClockSeconds() const = 0;

    // -----------------------------------------------------------------------
    // Delegates — concrete impl broadcasts these; subsystem subscribes.
    // These are plain multicast delegates (non-dynamic) — C++ only.
    // -----------------------------------------------------------------------

    /** Fires on the game thread when room join completes successfully.
     *  Subsystem binds to transition CurrentVoiceMode to LiveKit. */
    FOnLiveKitJoined OnJoined;

    /** Fires on the game thread when the room is cleanly disconnected.
     *  Subsystem binds to transition CurrentVoiceMode to WebSocket. */
    FOnLiveKitLeft OnLeft;

    /** Fires on the game thread when a connection or join error occurs.
     *  Subsystem binds to log the error and keep CurrentVoiceMode as WebSocket. */
    FOnLiveKitConnectionError OnConnectionError;
};

// ---------------------------------------------------------------------------
// IEstuaryLiveKitVideo — v2 stub (no v1 consumers).
// ---------------------------------------------------------------------------

/**
 * UEstuaryLiveKitVideo — UObject half of the video interface pair.
 * Not used in v1. Declared now so the interface signature is stable for v2.
 */
UINTERFACE(NotBlueprintable)
class ESTUARY_API UEstuaryLiveKitVideo : public UInterface
{
    GENERATED_BODY()
};

/**
 * IEstuaryLiveKitVideo — interface for optional LiveKit WebRTC video (v2).
 * Not implemented in v1. All methods are declared for API stability.
 * The concrete v2 impl will live in EstuaryLiveKit module.
 */
class ESTUARY_API IEstuaryLiveKitVideo
{
    GENERATED_BODY()

public:
    /** v2: Start publishing a video track. Not implemented in v1. */
    virtual void StartVideo() = 0;
    /** v2: Stop publishing the video track. Not implemented in v1. */
    virtual void StopVideo() = 0;
};

// ---------------------------------------------------------------------------
// FEstuaryLiveKitBridge — service-locator singleton.
// ---------------------------------------------------------------------------

/**
 * Service-locator bridge for optional LiveKit implementations.
 *
 * The core Estuary module owns one of these as a value member inside UEstuarySubsystem.
 * The EstuaryLiveKit module registers concrete implementations in StartupModule.
 * Core code only ever calls through this bridge — never includes LiveKit headers.
 *
 * Usage (core code):
 *   FEstuaryLiveKitBridge& Bridge = Subsystem->GetLiveKitBridge();
 *   if (Bridge.IsAvailable())
 *   {
 *       IEstuaryLiveKitVoice* Voice = Bridge.GetVoice();
 *       if (Voice) { Voice->JoinRoom(Token, Url, Room); }
 *   }
 *
 * Thread safety: all methods MUST be called on the game thread.
 */
class ESTUARY_API FEstuaryLiveKitBridge
{
public:
    FEstuaryLiveKitBridge()  = default;
    ~FEstuaryLiveKitBridge() = default;

    // Non-copyable, non-movable (owned by UEstuarySubsystem as a value member).
    FEstuaryLiveKitBridge(const FEstuaryLiveKitBridge&)            = delete;
    FEstuaryLiveKitBridge& operator=(const FEstuaryLiveKitBridge&) = delete;
    FEstuaryLiveKitBridge(FEstuaryLiveKitBridge&&)                  = delete;
    FEstuaryLiveKitBridge& operator=(FEstuaryLiveKitBridge&&)       = delete;

    // -----------------------------------------------------------------------
    // Voice impl registration — called by EstuaryLiveKit module lifecycle.
    // -----------------------------------------------------------------------

    /**
     * Register a concrete voice implementation. Called from EstuaryLiveKit::StartupModule.
     *
     * The impl must remain alive for the plugin's lifetime. The EstuaryLiveKit module
     * owns the impl as a UObject with AddToRoot(); do not call with a stack-allocated impl.
     * Must be called on the game thread.
     *
     * @param Impl  Non-null pointer to a concrete IEstuaryLiveKitVoice implementation.
     */
    void RegisterVoiceImpl(IEstuaryLiveKitVoice* Impl)
    {
        check(Impl != nullptr);
        VoiceImpl = Impl;
    }

    /**
     * Unregister the voice implementation. Called from EstuaryLiveKit::ShutdownModule.
     * After this call, IsAvailable() returns false and GetVoice() returns nullptr.
     * Must be called on the game thread. Safe to call when no impl is registered.
     */
    void UnregisterVoiceImpl()
    {
        VoiceImpl = nullptr;
    }

    /**
     * Returns true when WITH_ESTUARY_LIVEKIT is defined (compile-time) AND a voice
     * implementation has been registered at runtime.
     *
     * BOTH conditions must hold:
     *   - WITH_ESTUARY_LIVEKIT=1 is defined by EstuaryLiveKit.Build.cs when
     *     ThirdParty/livekit-client-sdk-cpp is present.
     *   - RegisterVoiceImpl has been called from EstuaryLiveKit::StartupModule.
     *
     * A machine without ThirdParty/livekit-client-sdk-cpp will never reach
     * RegisterVoiceImpl, so this naturally returns false in that scenario.
     *
     * Must be called on the game thread.
     */
    bool IsAvailable() const
    {
#if WITH_ESTUARY_LIVEKIT
        return VoiceImpl != nullptr;
#else
        return false;
#endif
    }

    /**
     * Get the registered voice implementation.
     * Returns nullptr when not available (IsAvailable() == false).
     * Callers MUST check IsAvailable() or null-check the return before invoking methods.
     * Must be called on the game thread.
     */
    IEstuaryLiveKitVoice* GetVoice() const
    {
        return VoiceImpl;
    }

    // -----------------------------------------------------------------------
    // Video impl registration (v2 — not used in v1).
    // -----------------------------------------------------------------------

    /**
     * Register a concrete video implementation (v2 — not used in v1).
     * Declared now so the signature is stable when v2 implements it.
     * Must be called on the game thread.
     */
    void RegisterVideoImpl(IEstuaryLiveKitVideo* Impl)
    {
        VideoImpl = Impl;
    }

    /** Unregister the video implementation. Must be called on the game thread. */
    void UnregisterVideoImpl()
    {
        VideoImpl = nullptr;
    }

    /**
     * Get the registered video implementation.
     * Returns nullptr in v1 (no video support until v2).
     * Must be called on the game thread.
     */
    IEstuaryLiveKitVideo* GetVideo() const
    {
        return VideoImpl;
    }

private:
    /**
     * Raw pointer to the registered voice impl.
     * Lifetime is managed by EstuaryLiveKit module (UObject + AddToRoot).
     * We do NOT own this pointer. Set to nullptr in UnregisterVoiceImpl.
     *
     * T-08-01: UnregisterVoiceImpl() sets this to nullptr before the impl is destroyed;
     * IsAvailable() returns false; callers null-check return of GetVoice() before use.
     */
    IEstuaryLiveKitVoice* VoiceImpl = nullptr;

    /** v2 video impl. nullptr in v1. */
    IEstuaryLiveKitVideo* VideoImpl = nullptr;
};
