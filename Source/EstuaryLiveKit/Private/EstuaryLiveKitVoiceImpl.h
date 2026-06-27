// EstuaryLiveKitVoiceImpl.h — Concrete UObject implementation of IEstuaryLiveKitVoice.
// Pitfall 23: #pragma once, IWYU, no CoreMinimal.h.
// Pitfall 7:  Public header still UCLASS-only — livekit/* includes live behind
//             WITH_ESTUARY_LIVEKIT in this Private header (never the core Estuary module).

#pragma once

#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/WeakObjectPtr.h"
#include "Async/Future.h"
#include "EstuaryLiveKitBridge.h"

#if WITH_ESTUARY_LIVEKIT
    #pragma push_macro("check")
    #pragma push_macro("PI")
    #undef check
    #undef PI
    #include <atomic>
    #include <memory>
    #include <string>
    #include <vector>
    #include "livekit/livekit.h"
    #include "livekit/room.h"
    #include "livekit/room_delegate.h"
    #include "livekit/room_event_types.h"
    #include "livekit/audio_source.h"
    #include "livekit/audio_stream.h"
    #include "livekit/audio_processing_module.h"
    #include "livekit/local_audio_track.h"
    #include "livekit/local_participant.h"
    #include "livekit/track.h"
    #pragma pop_macro("PI")
    #pragma pop_macro("check")
#endif

#include "EstuaryLiveKitVoiceImpl.generated.h"

#if WITH_ESTUARY_LIVEKIT
class UEstuaryLiveKitVoiceImpl;

/**
 * Concrete livekit::RoomDelegate that forwards SDK callbacks back to
 * UEstuaryLiveKitVoiceImpl on the game thread. Lifetime is managed by the impl
 * via std::unique_ptr — kept alive as long as LkRoom is alive.
 *
 * Threading: the SDK invokes these overrides on its internal signal_thread or
 * dedicated reader threads. Every override that touches UObject state MUST capture
 * WeakSelf and marshal to GameThread via AsyncTask (Pitfall 1).
 */
struct FEstuaryLkDelegate : public livekit::RoomDelegate
{
    TWeakObjectPtr<UEstuaryLiveKitVoiceImpl> WeakSelf;

    explicit FEstuaryLkDelegate(TWeakObjectPtr<UEstuaryLiveKitVoiceImpl> InWeakSelf)
        : WeakSelf(MoveTemp(InWeakSelf)) {}

    // RoomDelegate overrides — implementations live in the .cpp.
    void onDisconnected(livekit::Room& Room, const livekit::DisconnectedEvent& Event) override;
    void onTrackSubscribed(livekit::Room& Room, const livekit::TrackSubscribedEvent& Event) override;
    void onTrackUnsubscribed(livekit::Room& Room, const livekit::TrackUnsubscribedEvent& Event) override;
};
#endif // WITH_ESTUARY_LIVEKIT

/**
 * Concrete UObject implementation of IEstuaryLiveKitVoice.
 *
 * Wraps livekit::Room (from livekit/client-sdk-cpp) to:
 *   - Connect to a room via JoinRoom(token, url, room) on a background thread.
 *   - Publish a local microphone audio track via PushMicPcm (Task C).
 *   - Subscribe to the remote bot audio track via setOnAudioFrameCallback (Task D).
 *   - Marshal LiveKit callbacks to game thread and broadcast our delegates.
 *   - RAII teardown via std::unique_ptr<livekit::Room>: ~Room IS the disconnect.
 *
 * Registered with FEstuaryLiveKitBridge in EstuaryLiveKitModule::StartupModule via AddToRoot.
 * Unregistered and RemoveFromRoot'd in EstuaryLiveKitModule::ShutdownModule.
 */
UCLASS()
class UEstuaryLiveKitVoiceImpl : public UObject, public IEstuaryLiveKitVoice
{
    GENERATED_BODY()

public:
    UEstuaryLiveKitVoiceImpl();
    virtual ~UEstuaryLiveKitVoiceImpl() override;

    // UObject
    virtual void BeginDestroy() override;

    // IEstuaryLiveKitVoice — all must be called on the game thread.
    virtual void JoinRoom(const FString& Token, const FString& Url, const FString& Room) override;
    virtual void LeaveRoom() override;
    virtual bool IsJoined() const override;
    virtual void SetMuted(bool bMuted) override;
    virtual void PushMicPcm(const int16* Pcm16Samples, int32 NumSamples, int32 SampleRateHz) override;
    virtual void PushPlaybackPcm(const int16* Pcm16Samples, int32 NumSamples, int32 SampleRateHz) override;
    virtual double GetPlayoutClockSeconds() const override;

#if WITH_ESTUARY_LIVEKIT
    // Friend the delegate so it can write to the atomic clock counters and reach private state.
    friend struct FEstuaryLkDelegate;
#endif

private:
    /** True after successful room connection, false after LeaveRoom or error. */
    bool bIsJoined = false;

    /** Current mute state for the local audio track. Cached to apply after JoinRoom if set
     *  before the room is connected. */
    bool bIsMuted = false;

#if WITH_ESTUARY_LIVEKIT
    // Lifetime tracks the LiveKit session. unique_ptr destructor IS the disconnect
    // (RESEARCH section 6 — no Disconnect() method exists on Room). full type required
    // here because unique_ptr's destructor needs the complete type at instantiation.
    std::unique_ptr<livekit::Room> LkRoom;
    std::unique_ptr<FEstuaryLkDelegate> LkDelegate;
    std::shared_ptr<livekit::AudioSource>     LkAudioSource;
    std::shared_ptr<livekit::LocalAudioTrack> LkMicTrack;
    FString BotIdentity;

    // Bot-audio reader teardown ordering (PIE-stop crash fix, 2026-06-10): the impl
    // owns the subscribed AudioStream and the reader thread's future so TeardownRoomGT
    // can close() the stream and JOIN the reader BEFORE ~Room runs. The previous design
    // relied on ~Room invalidating the track's FFI handles to make the reader's blocked
    // Stream->read() return false — that races the in-flight FFI read against Room
    // destruction on the thread pool, and crashed reproducibly on PIE stop while bot
    // audio was streaming. Game thread only.
    std::shared_ptr<livekit::AudioStream> LkBotStream;
    TFuture<void> LkReaderFuture;

    // AEC (Pitfall 27): standalone WebRTC APM. The C++ SDK's AudioSource applies NO
    // processing to custom-pushed PCM, and UE renders remote audio through its own
    // engine, so WebRTC's built-in echo canceller never sees either stream. We run the
    // APM explicitly, entirely on the game thread (both feeds arrive there, so no
    // locking): PushPlaybackPcm feeds processReverseStream with the far-end reference
    // (paced by the audio player's consumption clock), and PushMicPcm runs
    // processStream on each exact-10ms window before captureFrame.
    //
    // LIFETIME (Pitfall 28 — PIE-stop crash #3 root cause): the APM is PROCESS-LIFETIME.
    // Created on first join, REUSED across sessions, and NEVER FFI-disposed — dropping
    // the APM handle in livekit_ffi v0.3.4 hangs/aborts the process (teardown task froze
    // inside ~AudioProcessingModule with step traces on either side; the two earlier
    // PIE-stop crashes match the same dispose racing ~Room on the GT). On shutdown or
    // APM error the unique_ptr is release()'d — ONE leaked APM per process, reclaimed at
    // exit. Per-session enablement lives in bApmEnabledForSession, not in LkApm's null-ness.
    std::unique_ptr<livekit::AudioProcessingModule> LkApm;
    bool bApmEnabledForSession = false;    // settings gate, set per JoinRoom, cleared on teardown
    std::vector<int16_t> ApmMicAccum;      // mic samples awaiting a full 10ms window (160 @ 16 kHz)
    std::vector<int16_t> ApmReverseAccum;  // playback samples awaiting a full 10ms window
    int32 ApmReverseRateHz = 0;            // rate of ApmReverseAccum contents; accum resets on change
    int32 ApmStreamDelayMs = 50;           // cached from UEstuaryDeveloperSettings at JoinRoom
    int32 ApmWarnedBadRateHz = 0;          // last rate we warned about (10ms framing needs rate % 100 == 0)

    // Atomic playout-clock state (LK-05). Both written from the SDK reader thread in
    // Task D's audio frame callback; read from the game thread by GetPlayoutClockSeconds().
    //   LkPlayoutSamplesTotal — cumulative samples-per-channel received (monotonic, reset on Leave).
    //   LkPlayoutSeconds      — derived elapsed seconds = total / sample_rate, refreshed per frame.
    std::atomic<uint64> LkPlayoutSamplesTotal {0};
    std::atomic<double> LkPlayoutSeconds      {0.0};

    // BL-01 race guard: bConnectInFlight is set true by JoinRoom before dispatching the
    // blocking Room::Connect to the thread pool, and cleared by the worker on completion.
    // LeaveRoom / BeginDestroy check it — if the worker is still inside Connect, they set
    // bLeavePending and return; the worker's GT-marshalled completion lambda then performs
    // teardown. This prevents two threads from touching LkRoom concurrently.
    std::atomic<bool> bConnectInFlight {false};
    std::atomic<bool> bLeavePending    {false};
#else
    void* LkRoomPtr = nullptr;   // stub-mode placeholder
#endif

    /** Marshal and broadcast OnJoined. Must be called on game thread. */
    void BroadcastJoined();

    /** Marshal and broadcast OnLeft. Must be called on game thread. */
    void BroadcastLeft();

    /** Marshal and broadcast OnConnectionError. Must be called on game thread. */
    void BroadcastError(const FString& Message);

#if WITH_ESTUARY_LIVEKIT
    /** Apply mute state to LkMicTrack (no-op if not yet published). Game thread only. */
    void ApplyMute(bool bMuted);

    /** GT helper: find the active character's audio player and push PCM into the ring buffer. */
    void RouteRemotePcmToPlayer(const TArray<uint8>& PcmBytes, int32 SampleRate);

    /** GT helper: synchronous teardown of LK callbacks, mic track, audio source, room and
     *  delegate (in that order). Called from LeaveRoom and from JoinRoom's completion lambda
     *  when bLeavePending was set during a Connect-in-flight race (BL-01). */
    void TeardownRoomGT(bool bBroadcastLeft);
#endif
};
