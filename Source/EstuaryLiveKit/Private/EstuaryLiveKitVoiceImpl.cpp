// EstuaryLiveKitVoiceImpl.cpp — livekit::Room wrapper implementation.
//
// Pitfall 1:  All UObject touches on game thread. LK callbacks use AsyncTask(GameThread).
// Pitfall 3:  TWeakObjectPtr captures in all LK callbacks.
// Pitfall 7:  livekit/* includes only here (and in the gated header) — never in core Estuary.
// Pitfall 23: IWYU. No CoreMinimal.h.

#include "EstuaryLiveKitVoiceImpl.h"
#include "EstuaryLog.h"
#include "Async/Async.h"
#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Timespan.h"

#define LK_TRACE(Fmt, ...) UE_LOG(LogEstuary, Log, TEXT("[EstuaryLiveKit] " Fmt), ##__VA_ARGS__)

#if WITH_ESTUARY_LIVEKIT

#include "EstuarySubsystem.h"
#include "EstuaryCharacter.h"
#include "EstuaryAudioPlayerComponent.h"
#include "EstuaryDeveloperSettings.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

#pragma push_macro("check")
#pragma push_macro("PI")
#undef check
#undef PI
#include "livekit/audio_stream.h"
#pragma pop_macro("PI")
#pragma pop_macro("check")

// ============================================================================
// Pitfall 29 — cross-DLL heap discipline
// ============================================================================
// Every UE module overrides global operator new/delete (FMemory, PER_MODULE_BOILERPLATE),
// while the PREBUILT livekit.dll allocates std::string / std::vector buffers on the MSVC
// CRT heap. Destroying such an object in THIS module frees a CRT block through FMemory —
// silent heap corruption, then an AV at some later alloc/free (proven by PIE-test crash
// callstack: reader-exit ~AudioFrameEvent → vector<short>::_Tidy → our operator delete).
// shared_ptrs returned by the SDK are SAFE (their deleter is captured inside livekit.dll);
// std::string / std::vector returned BY VALUE are NOT — never let one be destroyed on our
// side. Pattern: materialize the return value straight into a heap object (C++17 guaranteed
// elision — no temporary dies our side), copy the chars out, and deliberately leak it.
// Cost: a few tiny strings + one audio frame per session.

static FString LkLeakString(std::string* LeakedUtf8)
{
    return FString(UTF8_TO_TCHAR(LeakedUtf8->c_str()));   // LeakedUtf8 intentionally never freed
}

// ============================================================================
// FEstuaryLkDelegate — RoomDelegate forwarder
// ============================================================================

void FEstuaryLkDelegate::onDisconnected(livekit::Room& /*Room*/, const livekit::DisconnectedEvent& Event)
{
    const int32 ReasonCode = static_cast<int32>(Event.reason);
    AsyncTask(ENamedThreads::GameThread,
        [LocalWeak = WeakSelf, ReasonCode]()
        {
            UEstuaryLiveKitVoiceImpl* Self = LocalWeak.Get();
            if (!Self) { return; }
            // If we're still marked joined, the disconnect came from the server (not LeaveRoom).
            // Broadcast Left so the subsystem flips back to WebSocket mode and Socket.IO emits
            // livekit_leave on the next tick.
            if (Self->bIsJoined)
            {
                LK_TRACE("onDisconnected reason=%d — server-initiated", ReasonCode);
                Self->bIsJoined = false;
                Self->BroadcastLeft();
            }
        });
}

void FEstuaryLkDelegate::onTrackSubscribed(livekit::Room& /*Room*/, const livekit::TrackSubscribedEvent& Event)
{
    if (!Event.track || Event.track->kind() != livekit::TrackKind::KIND_AUDIO || !Event.participant)
    {
        return;
    }

    // Capture identity, track name, sid, and source values by value off the signal thread.
    // Event.participant / Event.track / Event.publication are owned by Room and valid for the
    // duration of this call, but every UObject touch is marshalled to GT (Pitfall 1).
    // Pitfall 29: these getters return std::string by value with livekit.dll-allocated
    // buffers — go straight to leaked heap objects + FString copies, never our-side dtors.
    const FString ParticipantId = LkLeakString(new std::string(Event.participant->identity()));
    const FString TrackName     = LkLeakString(new std::string(Event.track->name()));
    const FString TrackSid      = LkLeakString(new std::string(Event.track->sid()));

    // Diagnostic: the prior session traced silent bot-voice loss to setOnAudioFrameCallback's
    // source-based filter not matching. Log both Track::source() (optional) and
    // RemoteTrackPublication::source() (non-optional) so we can verify which value is on the
    // wire and confirm the track-name fallback below is correct (-1 means optional was nullopt).
    const auto TrackSourceOpt = Event.track->source();
    const int TrackSourceVal  = TrackSourceOpt.has_value() ? static_cast<int>(*TrackSourceOpt) : -1;
    const int PubSourceVal    = Event.publication
        ? static_cast<int>(Event.publication->source())
        : -1;
    LK_TRACE("onTrackSubscribed audio participant='%s' name='%s' sid='%s' track_source=%d pub_source=%d",
        *ParticipantId,
        *TrackName,
        *TrackSid,
        TrackSourceVal,
        PubSourceVal);

    AsyncTask(ENamedThreads::GameThread,
        [LocalWeak = WeakSelf,
         Track = Event.track,
         ParticipantId]()
        {
            UEstuaryLiveKitVoiceImpl* Self = LocalWeak.Get();
            if (!Self || !Self->LkRoom) { return; }

            // Cache identity (still useful for log/leave correlation).
            Self->BotIdentity = ParticipantId;

            TWeakObjectPtr<UEstuaryLiveKitVoiceImpl> WeakImpl = LocalWeak;

            // Bypass Room::setOnAudioFrameCallback / SubscriptionThreadDispatcher entirely —
            // diagnostics in this session showed that the dispatcher's `(participant, source)`
            // and `(participant, track_name)` registrations both successfully record but never
            // invoke the callback for our prebuilt LK C++ SDK. AudioStream::fromTrack binds
            // a stream directly to the subscribed Track (no enum/name matching), giving us a
            // pull-based reader we drive from a dedicated thread.
            std::shared_ptr<livekit::AudioStream> Stream;
            try
            {
                Stream = livekit::AudioStream::fromTrack(Track, livekit::AudioStream::Options{});
            }
            catch (const std::exception& e)
            {
                UE_LOG(LogEstuary, Error,
                    TEXT("[EstuaryLiveKit] AudioStream::fromTrack threw: %s"), UTF8_TO_TCHAR(e.what()));
                return;
            }
            if (!Stream)
            {
                UE_LOG(LogEstuary, Error,
                    TEXT("[EstuaryLiveKit] AudioStream::fromTrack returned null"));
                return;
            }

            LK_TRACE("AudioStream created for bot track — spawning reader thread");

            // PIE-stop crash fix (2026-06-10): a previous stream (server moved the track /
            // re-subscribe) must be closed and its reader joined before we replace it, or
            // its read() loop races the eventual ~Room exactly like the teardown bug below.
            if (Self->LkBotStream)
            {
                try { Self->LkBotStream->close(); } catch (...) { /* swallow */ }
                if (Self->LkReaderFuture.IsValid())
                {
                    Self->LkReaderFuture.WaitFor(FTimespan::FromSeconds(1.0));
                }
            }

            // Reader thread: blocks on Stream->read() until EOS / close. The Stream and Track
            // shared_ptrs are captured by value so they outlive the GT lambda. Teardown is
            // EXPLICIT (PIE-stop crash fix, 2026-06-10): TeardownRoomGT close()s LkBotStream
            // (which wakes the blocked read() → returns false) and joins this thread via
            // LkReaderFuture BEFORE ~Room — an in-flight FFI read must never overlap Room
            // destruction. ~AudioStream() still calls close() as a backstop.
            Self->LkBotStream = Stream;
            Self->LkReaderFuture = Async(EAsyncExecution::Thread, [Stream, Track, WeakImpl]()
            {
                int FrameNum = 0;
                // Pitfall 29: read() (re)fills the event's vector with livekit.dll-allocated
                // buffers; a stack-local Ev would cross-free the final buffer in OUR dtor at
                // scope exit (this exact AV was caught by the autonomous PIE test). Heap-
                // allocate and deliberately leak it (~one audio frame per session); all
                // in-loop buffer swaps happen inside livekit.dll via read()'s move-assign.
                livekit::AudioFrameEvent* Ev = new livekit::AudioFrameEvent();
                while (Stream->read(*Ev))
                {
                    const livekit::AudioFrame& Frame = Ev->frame;
                    const std::vector<int16_t>& Data = Frame.data();
                    const int32 SamplesPerChannel = Frame.samples_per_channel();
                    const int32 SampleRate = Frame.sample_rate();

                    if (FrameNum < 5 || FrameNum % 100 == 0)
                    {
                        UE_LOG(LogEstuary, Log,
                            TEXT("[EstuaryLiveKit] AudioStream frame #%d: SR=%d samples=%d bytes=%d"),
                            FrameNum, SampleRate, SamplesPerChannel,
                            static_cast<int>(Data.size()) * static_cast<int>(sizeof(int16)));
                    }
                    ++FrameNum;

                    if (UEstuaryLiveKitVoiceImpl* SelfFromCb = WeakImpl.Get())
                    {
                        if (SampleRate > 0 && SamplesPerChannel > 0)
                        {
                            const uint64 Prev = SelfFromCb->LkPlayoutSamplesTotal.fetch_add(
                                static_cast<uint64>(SamplesPerChannel), std::memory_order_acq_rel);
                            const double Now = static_cast<double>(Prev + SamplesPerChannel) /
                                               static_cast<double>(SampleRate);
                            SelfFromCb->LkPlayoutSeconds.store(Now, std::memory_order_release);
                        }
                    }

                    const int32 NumBytes = static_cast<int32>(Data.size()) * static_cast<int32>(sizeof(int16));
                    if (NumBytes <= 0) { continue; }
                    TArray<uint8> PcmBytes;
                    PcmBytes.SetNumUninitialized(NumBytes);
                    FMemory::Memcpy(PcmBytes.GetData(), Data.data(), NumBytes);

                    AsyncTask(ENamedThreads::GameThread,
                        [WeakImpl, PcmBytes = MoveTemp(PcmBytes), SampleRate]()
                        {
                            UEstuaryLiveKitVoiceImpl* SelfGT = WeakImpl.Get();
                            if (!SelfGT) { return; }
                            SelfGT->RouteRemotePcmToPlayer(PcmBytes, SampleRate);
                        });
                }
                UE_LOG(LogEstuary, Log,
                    TEXT("[EstuaryLiveKit] AudioStream reader exited (after %d frames)"), FrameNum);
                // Ev intentionally leaked — Pitfall 29.
            });
        });
}

void FEstuaryLkDelegate::onTrackUnsubscribed(livekit::Room& /*Room*/, const livekit::TrackUnsubscribedEvent& Event)
{
    if (!Event.track || Event.track->kind() != livekit::TrackKind::KIND_AUDIO || !Event.participant)
    {
        return;
    }
    // Pitfall 29: leak the by-value string, never destroy it our side.
    const FString ParticipantId = LkLeakString(new std::string(Event.participant->identity()));
    LK_TRACE("onTrackUnsubscribed audio participant='%s'", *ParticipantId);
    // Frame callback cleanup happens in LeaveRoom — leaving the registration alive here so a
    // brief unsubscribe-then-resubscribe cycle (server moves a track between participants)
    // doesn't drop a frame.
}

// ============================================================================
// UEstuaryLiveKitVoiceImpl — implementation (WITH_ESTUARY_LIVEKIT=1)
// ============================================================================

UEstuaryLiveKitVoiceImpl::UEstuaryLiveKitVoiceImpl()
    : bIsJoined(false)
    , bIsMuted(false)
{
}

UEstuaryLiveKitVoiceImpl::~UEstuaryLiveKitVoiceImpl()
{
    // BeginDestroy is the canonical UObject teardown hook; ~UObject runs after that.
    // If BeginDestroy fired and reset LkRoom, this is a no-op. If we somehow get here
    // with an active room (shouldn't happen — UE always calls BeginDestroy first),
    // RAII still wins: ~unique_ptr<Room> blocks here briefly.
}

void UEstuaryLiveKitVoiceImpl::BeginDestroy()
{
    // CONTEXT mandatory cleanup hook #1 — synchronous teardown to prevent quota leak.
    // BL-01: ~UObject (and thus ~unique_ptr<Room>) runs synchronously after BeginDestroy
    // returns, so we cannot defer to a GT-marshalled callback like LeaveRoom does — by the
    // time it would fire, the Room is already being destroyed on the worker. Bounded
    // spin-wait until the worker exits Connect (cap ~5s; better to stall PIE-stop a beat
    // than UAF a Room mid-FFI-call).
    if (bConnectInFlight.load(std::memory_order_acquire))
    {
        LK_TRACE("BeginDestroy — waiting for in-flight Connect to return");
        const double Deadline = FPlatformTime::Seconds() + 5.0;
        while (bConnectInFlight.load(std::memory_order_acquire))
        {
            if (FPlatformTime::Seconds() > Deadline)
            {
                UE_LOG(LogEstuary, Error,
                    TEXT("[EstuaryLiveKit] BeginDestroy timed out waiting for Connect — proceeding (Room may leak)"));
                break;
            }
            FPlatformProcess::Sleep(0.01f);
        }
    }

    if (LkRoom)
    {
        LK_TRACE("BeginDestroy — synchronous Room teardown");
        TeardownRoomGT(/*bBroadcastLeft=*/false);
    }

    // Pitfall 28: the APM is never FFI-disposed — release (one intentional leak per
    // process, reclaimed at exit). Letting ~unique_ptr run here would send the dispose
    // that hangs/aborts the process.
    if (LkApm) { LkApm.release(); }

    Super::BeginDestroy();
}

void UEstuaryLiveKitVoiceImpl::JoinRoom(const FString& Token, const FString& Url, const FString& Room)
{
    check(IsInGameThread());

    if (bIsJoined || LkRoom)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[EstuaryLiveKit] JoinRoom called while already joined — call LeaveRoom first."));
        return;
    }

    // T-08-03: Never log Token (JWT credential).
    LK_TRACE("JoinRoom — url=%s room=%s", *Url, *Room);

    TWeakObjectPtr<UEstuaryLiveKitVoiceImpl> WeakThis(this);
    std::string UrlUtf8   = TCHAR_TO_UTF8(*Url);
    std::string TokenUtf8 = TCHAR_TO_UTF8(*Token);

    auto NewRoom = std::make_unique<livekit::Room>();
    livekit::Room* RawRoom = NewRoom.get();
    LkRoom = std::move(NewRoom);

    LkDelegate = std::make_unique<FEstuaryLkDelegate>(WeakThis);
    LkRoom->setDelegate(LkDelegate.get());

    // Reset clock state in case a prior session left non-zero values.
    LkPlayoutSamplesTotal.store(0, std::memory_order_release);
    LkPlayoutSeconds.store(0.0, std::memory_order_release);

    // BL-01: mark Connect in flight so LeaveRoom / BeginDestroy defer teardown.
    bLeavePending.store(false, std::memory_order_release);
    bConnectInFlight.store(true, std::memory_order_release);

    Async(EAsyncExecution::ThreadPool,
        [WeakThis, RawRoom, UrlUtf8 = std::move(UrlUtf8), TokenUtf8 = std::move(TokenUtf8)]() mutable
        {
            livekit::RoomOptions Options;   // defaults: auto_subscribe=true, single_peer_connection=true
            bool bConnected = false;
            FString FailMsg;
            try
            {
                bConnected = RawRoom->Connect(UrlUtf8, TokenUtf8, Options);
                if (!bConnected) { FailMsg = TEXT("livekit::Room::Connect returned false"); }
            }
            catch (const std::exception& e)
            {
                FailMsg = FString::Printf(TEXT("livekit::Room::Connect threw: %s"), UTF8_TO_TCHAR(e.what()));
            }
            catch (...)
            {
                FailMsg = TEXT("livekit::Room::Connect threw unknown exception");
            }

            // BL-01: clear the in-flight flag HERE on the worker — its last touch of the
            // object — BEFORE queuing the GT completion task. BeginDestroy's spin-wait
            // runs ON the game thread without pumping the task graph, so a GT-marshalled
            // clear can never be observed by it (it would always burn the full 5 s
            // timeout, then race ~Room against a possibly still-blocked Connect).
            // Safe: the spin guarantees the object stays alive while the flag is true,
            // and after this store the worker performs no further object access (the GT
            // lambda re-resolves WeakThis itself). GetEvenIfUnreachable: during the
            // shutdown purge the object is marked unreachable while BeginDestroy spins —
            // plain Get() would return null exactly when the clear matters most.
            if (UEstuaryLiveKitVoiceImpl* Pinned = WeakThis.GetEvenIfUnreachable())
            {
                Pinned->bConnectInFlight.store(false, std::memory_order_release);
            }

            AsyncTask(ENamedThreads::GameThread,
                [WeakThis, bConnected, FailMsg]()
                {
                    UEstuaryLiveKitVoiceImpl* Self = WeakThis.Get();
                    // Re-clear is a harmless idempotent no-op (worker already cleared it);
                    // kept so any LeaveRoom called after this point takes the sync path
                    // even if the worker-side weak resolve failed.
                    if (Self) { Self->bConnectInFlight.store(false, std::memory_order_release); }

                    if (!Self) { return; }   // GC'd — BeginDestroy already cleaned up

                    // BL-01: a Leave/BeginDestroy fired while Connect was in flight. Tear down now.
                    if (Self->bLeavePending.load(std::memory_order_acquire))
                    {
                        Self->bLeavePending.store(false, std::memory_order_release);
                        LK_TRACE("JoinRoom completion — bLeavePending set, performing deferred teardown");
                        if (Self->LkRoom)
                        {
                            // If Connect succeeded we own a live Room; teardown broadcasts Left so
                            // the subsystem flips back to WebSocket. If it failed, Broadcast Error
                            // instead so callers see the failure that was masked by the deferred path.
                            if (bConnected) { Self->TeardownRoomGT(/*bBroadcastLeft=*/true); }
                            else            { Self->TeardownRoomGT(/*bBroadcastLeft=*/false);
                                              Self->BroadcastError(FailMsg); }
                        }
                        return;
                    }

                    if (!bConnected)
                    {
                        LK_TRACE("JoinRoom failed — %s", *FailMsg);
                        Self->LkMicTrack.reset();
                        Self->LkAudioSource.reset();
                        Self->LkRoom.reset();
                        Self->LkDelegate.reset();
                        Self->bIsJoined = false;
                        Self->BroadcastError(FailMsg);
                        return;
                    }

                    // Mic publish (Task C) — done on GT after Connect succeeds, before bIsJoined flip.
                    try
                    {
                        const int32 SampleRate = 16000;   // matches EstuaryDeveloperSettings::AudioSampleRate (animation mode)
                        Self->LkAudioSource = std::make_shared<livekit::AudioSource>(
                            SampleRate, /*num_channels=*/1, /*queue_size_ms=*/0);
                        Self->LkMicTrack = livekit::LocalAudioTrack::createLocalAudioTrack(
                            "estuary-mic", Self->LkAudioSource);

                        livekit::LocalParticipant* Local = Self->LkRoom->localParticipant();
                        if (!Local) { throw std::runtime_error("local participant null after Connect"); }

                        Local->publishAudioTrack(
                            "estuary-mic", Self->LkAudioSource, livekit::TrackSource::SOURCE_MICROPHONE);
                    }
                    catch (const std::exception& e)
                    {
                        const FString Msg = FString::Printf(
                            TEXT("LiveKit mic publish failed: %s"), UTF8_TO_TCHAR(e.what()));
                        UE_LOG(LogEstuary, Error, TEXT("[EstuaryLiveKit] %s"), *Msg);
                        Self->LkMicTrack.reset();
                        Self->LkAudioSource.reset();
                        Self->LkRoom.reset();
                        Self->LkDelegate.reset();
                        Self->bIsJoined = false;
                        Self->BroadcastError(Msg);
                        return;
                    }

                    // AEC (Pitfall 27): arm the APM for this session. Failure is non-fatal —
                    // voice still works, just without echo cancellation. The APM itself is
                    // process-lifetime and never FFI-disposed (Pitfall 28) — see the header.
                    const UEstuaryDeveloperSettings* Settings = UEstuaryDeveloperSettings::Get();
                    Self->bApmEnabledForSession = (Settings && Settings->bLiveKitEchoCancellation);
                    if (Self->bApmEnabledForSession)
                    {
                        try
                        {
                            if (!Self->LkApm)
                            {
                                livekit::AudioProcessingModule::Options ApmOptions;
                                ApmOptions.echo_cancellation = true;
                                ApmOptions.high_pass_filter  = true;
                                Self->LkApm = std::make_unique<livekit::AudioProcessingModule>(ApmOptions);
                                LK_TRACE("AEC: APM created (process-lifetime, Pitfall 28)");
                            }
                            Self->ApmStreamDelayMs = Settings->AecStreamDelayMs;
                            Self->LkApm->setStreamDelayMs(Self->ApmStreamDelayMs);
                            LK_TRACE("AEC enabled for session (stream delay %d ms)",
                                Self->ApmStreamDelayMs);
                        }
                        catch (const std::exception& e)
                        {
                            UE_LOG(LogEstuary, Warning,
                                TEXT("[EstuaryLiveKit] APM setup failed — continuing without AEC: %s"),
                                UTF8_TO_TCHAR(e.what()));
                            // Pitfall 28: release (intentional leak), never FFI-dispose.
                            Self->LkApm.release();
                            Self->bApmEnabledForSession = false;
                        }
                        Self->ApmMicAccum.clear();
                        Self->ApmReverseAccum.clear();
                        Self->ApmReverseRateHz = 0;
                    }

                    Self->bIsJoined = true;
                    LK_TRACE("JoinRoom success — broadcasting OnJoined");
                    Self->BroadcastJoined();

                    if (Self->bIsMuted) { Self->ApplyMute(true); }
                });
        });
}

void UEstuaryLiveKitVoiceImpl::LeaveRoom()
{
    check(IsInGameThread());

    // BL-01: if Connect is still running on the worker, do NOT touch LkRoom from the GT.
    // The worker's GT-marshalled completion lambda will observe bLeavePending and tear down.
    if (bConnectInFlight.load(std::memory_order_acquire))
    {
        bLeavePending.store(true, std::memory_order_release);
        LK_TRACE("LeaveRoom deferred — Connect still in flight");
        return;
    }

    if (!LkRoom) { return; }

    LK_TRACE("LeaveRoom");
    TeardownRoomGT(/*bBroadcastLeft=*/true);
}

void UEstuaryLiveKitVoiceImpl::TeardownRoomGT(bool bBroadcastLeft)
{
    check(IsInGameThread());
    if (!LkRoom) { return; }

    // Step 0 (PIE-stop crash fix, 2026-06-10): stop the bot-audio reader BEFORE anything
    // touches the Room. close() disposes the stream's FFI handle and wakes the reader's
    // blocked Stream->read() (returns false → loop exits); the bounded wait then JOINS the
    // reader thread so no FFI read is in flight when ~Room tears down the track handles.
    // The previous design relied on ~Room invalidating handles to unblock the reader —
    // that race crashed the editor on PIE stop while bot audio was streaming.
    if (LkBotStream)
    {
        try { LkBotStream->close(); }
        catch (const std::exception& e)
        {
            UE_LOG(LogEstuary, Warning,
                TEXT("[EstuaryLiveKit] AudioStream close threw during teardown: %s"),
                UTF8_TO_TCHAR(e.what()));
        }
    }
    if (LkReaderFuture.IsValid())
    {
        const bool bReaderExited = LkReaderFuture.WaitFor(FTimespan::FromSeconds(2.0));
        if (!bReaderExited)
        {
            UE_LOG(LogEstuary, Warning,
                TEXT("[EstuaryLiveKit] AudioStream reader did not exit within 2s — proceeding with Room teardown"));
        }
    }
    LkReaderFuture = TFuture<void>();
    LK_TRACE("Teardown: reader joined — handing FFI handles to teardown task");
    GLog->Flush();   // crash forensics: teardown crashes kill the process before buffered lines land

    // Steps 1+2 (PIE-stop crash fix #2, 2026-06-10): hand EVERY remaining FFI handle to
    // ONE thread-pool task and drop them there sequentially, children first, Room last.
    // The previous code queued ~Room on the pool and then dropped the mic-track /
    // audio-source / APM handles on the game thread — concurrent FFI disposes racing
    // ~Room. The second PIE-stop crash died exactly in that window (log ends right
    // after the reader join) with no UE crash report — consistent with a Rust-side
    // abort inside livekit_ffi.dll. NOTHING may dispose an FFI handle while ~Room runs.
    // The explicit unpublish (server-cleanup nicety) moved into the task for the same
    // reason; the Room is still alive at that point in the sequence.
    // NOTE: LkApm is deliberately NOT part of this teardown — it is process-lifetime
    // and never FFI-disposed (Pitfall 28: ~AudioProcessingModule froze this very task
    // in PIE-stop crash #3, between the "dropping APM" and "dropping audio stream"
    // traces, leaving ~Room unreached and the rtc engine alive attempting resumes).
    std::unique_ptr<livekit::Room> DepartingRoom               = std::move(LkRoom);
    std::unique_ptr<FEstuaryLkDelegate> DepartingDelegate      = std::move(LkDelegate);
    std::shared_ptr<livekit::LocalAudioTrack> DepartingTrack   = std::move(LkMicTrack);
    std::shared_ptr<livekit::AudioSource> DepartingSource      = std::move(LkAudioSource);
    std::shared_ptr<livekit::AudioStream> DepartingStream      = std::move(LkBotStream);
    Async(EAsyncExecution::ThreadPool,
        [DepartingRoom     = std::move(DepartingRoom),
         DepartingDelegate = std::move(DepartingDelegate),
         DepartingTrack    = std::move(DepartingTrack),
         DepartingSource   = std::move(DepartingSource),
         DepartingStream   = std::move(DepartingStream)]() mutable
        {
            if (DepartingRoom && DepartingTrack && DepartingTrack->publication())
            {
                try
                {
                    if (livekit::LocalParticipant* Local = DepartingRoom->localParticipant())
                    {
                        // Pitfall 29: sid() returns a by-value std::string — leak it.
                        std::string* Sid = new std::string(DepartingTrack->publication()->sid());
                        Local->unpublishTrack(*Sid);
                    }
                }
                catch (...) { /* swallow */ }
            }
            LK_TRACE("Teardown task: dropping mic track");   GLog->Flush();
            DepartingTrack.reset();
            LK_TRACE("Teardown task: dropping audio source"); GLog->Flush();
            DepartingSource.reset();
            LK_TRACE("Teardown task: dropping audio stream"); GLog->Flush();
            DepartingStream.reset();
            LK_TRACE("Teardown task: destroying Room");       GLog->Flush();
            // ~Room before ~delegate (Room holds the raw delegate ptr).
            DepartingRoom.reset();
            DepartingDelegate.reset();
            LK_TRACE("Teardown task: complete");              GLog->Flush();
        });

    ApmMicAccum.clear();
    ApmReverseAccum.clear();
    ApmReverseRateHz = 0;
    bApmEnabledForSession = false;
    BotIdentity.Empty();
    bIsJoined = false;
    LkPlayoutSamplesTotal.store(0, std::memory_order_release);
    LkPlayoutSeconds.store(0.0, std::memory_order_release);

    if (bBroadcastLeft) { BroadcastLeft(); }
}

bool UEstuaryLiveKitVoiceImpl::IsJoined() const
{
    return bIsJoined;
}

void UEstuaryLiveKitVoiceImpl::SetMuted(bool bMuted)
{
    check(IsInGameThread());
    bIsMuted = bMuted;
    if (LkMicTrack) { ApplyMute(bMuted); }
}

void UEstuaryLiveKitVoiceImpl::ApplyMute(bool bMuted)
{
    if (!LkMicTrack) { return; }
    try
    {
        if (bMuted) { LkMicTrack->mute(); } else { LkMicTrack->unmute(); }
    }
    catch (const std::exception& e)
    {
        UE_LOG(LogEstuary, Warning, TEXT("[EstuaryLiveKit] SetMuted threw: %s"), UTF8_TO_TCHAR(e.what()));
    }
}

void UEstuaryLiveKitVoiceImpl::PushMicPcm(const int16* Pcm16Samples, int32 NumSamples, int32 SampleRateHz)
{
    check(IsInGameThread());
    if (!LkAudioSource || !bIsJoined || NumSamples <= 0 || !Pcm16Samples) { return; }

    if (SampleRateHz != 16000)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[EstuaryLiveKit] PushMicPcm sample-rate mismatch (got %d, expected 16000) — dropping chunk"),
            SampleRateHz);
        return;
    }

    // AEC forward stream (Pitfall 27): run mic audio through the APM in exact-10ms
    // windows (160 samples @ 16 kHz) before publishing. The APM subtracts the echo it
    // learned from the far-end reference fed via PushPlaybackPcm. Remainder samples
    // wait in ApmMicAccum for the next chunk.
    if (LkApm && bApmEnabledForSession)
    {
        try
        {
            const int32 WindowSamples = SampleRateHz / 100;
            ApmMicAccum.insert(ApmMicAccum.end(), Pcm16Samples, Pcm16Samples + NumSamples);

            const size_t NumWindows = ApmMicAccum.size() / static_cast<size_t>(WindowSamples);
            if (NumWindows == 0) { return; }

            std::vector<int16_t> Processed;
            Processed.reserve(NumWindows * static_cast<size_t>(WindowSamples));
            for (size_t W = 0; W < NumWindows; ++W)
            {
                std::vector<int16_t> WindowBuf(
                    ApmMicAccum.begin() + W * WindowSamples,
                    ApmMicAccum.begin() + (W + 1) * WindowSamples);
                livekit::AudioFrame Frame(std::move(WindowBuf), SampleRateHz,
                    /*num_channels=*/1, /*samples_per_channel=*/WindowSamples);
                // WebRTC consumes the delay per ProcessStream call — re-set each window.
                LkApm->setStreamDelayMs(ApmStreamDelayMs);
                LkApm->processStream(Frame);
                Processed.insert(Processed.end(), Frame.data().begin(), Frame.data().end());
            }
            ApmMicAccum.erase(ApmMicAccum.begin(),
                ApmMicAccum.begin() + NumWindows * static_cast<size_t>(WindowSamples));

            const int32 ProcessedSamples = static_cast<int32>(Processed.size());
            livekit::AudioFrame OutFrame(std::move(Processed), SampleRateHz,
                /*num_channels=*/1, /*samples_per_channel=*/ProcessedSamples);
            LkAudioSource->captureFrame(OutFrame, /*timeout_ms=*/20);
            return;
        }
        catch (const std::exception& e)
        {
            // Fail open: disable AEC and fall through to the raw publish below so the
            // conversation keeps working (echo returns, audio does not die).
            // Pitfall 28: release (intentional leak), never FFI-dispose.
            UE_LOG(LogEstuary, Warning,
                TEXT("[EstuaryLiveKit] APM processStream failed — disabling AEC for this session: %s"),
                UTF8_TO_TCHAR(e.what()));
            LkApm.release();
            bApmEnabledForSession = false;
            ApmMicAccum.clear();
            ApmReverseAccum.clear();
        }
    }

    try
    {
        std::vector<int16_t> Buf(Pcm16Samples, Pcm16Samples + NumSamples);
        livekit::AudioFrame Frame(std::move(Buf), SampleRateHz, /*num_channels=*/1, /*samples_per_channel=*/NumSamples);
        LkAudioSource->captureFrame(Frame, /*timeout_ms=*/20);
    }
    catch (const std::exception& e)
    {
        UE_LOG(LogEstuary, Verbose,
            TEXT("[EstuaryLiveKit] captureFrame threw: %s"), UTF8_TO_TCHAR(e.what()));
    }
}

void UEstuaryLiveKitVoiceImpl::PushPlaybackPcm(const int16* Pcm16Samples, int32 NumSamples, int32 SampleRateHz)
{
    check(IsInGameThread());
    // AEC reverse stream (Pitfall 27): feed what the speakers are rendering into the
    // APM as the echo reference. Caller (UEstuaryAudioPlayerComponent) paces this to
    // the audio thread's actual consumption, so the timing matches real playout.
    if (!LkApm || !bApmEnabledForSession || !bIsJoined ||
        NumSamples <= 0 || !Pcm16Samples || SampleRateHz <= 0) { return; }

    // Exact-10ms framing requires the rate to be divisible by 100 (16k/24k/48k all are;
    // 22050/44100 are NOT). Without a reference feed, echo cancellation is inoperative —
    // say so loudly, once per offending rate, instead of failing silently.
    if (SampleRateHz % 100 != 0)
    {
        if (ApmWarnedBadRateHz != SampleRateHz)
        {
            ApmWarnedBadRateHz = SampleRateHz;
            UE_LOG(LogEstuary, Warning,
                TEXT("[EstuaryLiveKit] AEC reference inoperative: playback rate %d Hz is not divisible by 100 ")
                TEXT("(10ms APM framing). Echo cancellation will NOT work — use 16000/24000/48000 in ")
                TEXT("UEstuaryDeveloperSettings::AudioSampleRate."),
                SampleRateHz);
        }
        return;
    }

    // Playback rate changed (e.g. procedural wave reinit 16k → 48k): stale partial
    // window is meaningless at the new rate — drop it.
    if (SampleRateHz != ApmReverseRateHz)
    {
        ApmReverseAccum.clear();
        ApmReverseRateHz = SampleRateHz;
    }

    try
    {
        const int32 WindowSamples = SampleRateHz / 100;
        ApmReverseAccum.insert(ApmReverseAccum.end(), Pcm16Samples, Pcm16Samples + NumSamples);

        const size_t NumWindows = ApmReverseAccum.size() / static_cast<size_t>(WindowSamples);
        for (size_t W = 0; W < NumWindows; ++W)
        {
            std::vector<int16_t> WindowBuf(
                ApmReverseAccum.begin() + W * WindowSamples,
                ApmReverseAccum.begin() + (W + 1) * WindowSamples);
            livekit::AudioFrame Frame(std::move(WindowBuf), SampleRateHz,
                /*num_channels=*/1, /*samples_per_channel=*/WindowSamples);
            LkApm->processReverseStream(Frame);   // reference feed only — output discarded
        }
        if (NumWindows > 0)
        {
            ApmReverseAccum.erase(ApmReverseAccum.begin(),
                ApmReverseAccum.begin() + NumWindows * static_cast<size_t>(WindowSamples));
        }
    }
    catch (const std::exception& e)
    {
        // Pitfall 28: release (intentional leak), never FFI-dispose.
        UE_LOG(LogEstuary, Warning,
            TEXT("[EstuaryLiveKit] APM processReverseStream failed — disabling AEC for this session: %s"),
            UTF8_TO_TCHAR(e.what()));
        LkApm.release();
        bApmEnabledForSession = false;
        ApmMicAccum.clear();
        ApmReverseAccum.clear();
    }
}

double UEstuaryLiveKitVoiceImpl::GetPlayoutClockSeconds() const
{
    return LkPlayoutSeconds.load(std::memory_order_acquire);
}

void UEstuaryLiveKitVoiceImpl::RouteRemotePcmToPlayer(const TArray<uint8>& PcmBytes, int32 SampleRate)
{
    check(IsInGameThread());
    if (PcmBytes.Num() == 0) { return; }

    // Diagnostic: throttled trace per route call so we know GT marshalling worked.
    static int RouteCounter = 0;
    const int RouteNum = RouteCounter++;
    const bool bShouldLog = RouteNum < 5 || RouteNum % 100 == 0;

    UWorld* World = nullptr;
    if (GEngine)
    {
        for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
        {
            if (Ctx.World())
            {
                // Prefer PIE world over editor world (Editor=0, PIE/Game=3).
                if (Ctx.World()->WorldType == EWorldType::PIE || Ctx.World()->WorldType == EWorldType::Game)
                {
                    World = Ctx.World();
                    break;
                }
                if (!World) { World = Ctx.World(); }
            }
        }
    }
    if (!World)
    {
        if (bShouldLog) { LK_TRACE("RouteRemotePcmToPlayer #%d: NO World — dropping %d bytes", RouteNum, PcmBytes.Num()); }
        return;
    }

    // v1 LiveKit scope is single-character. Iterate AEstuaryCharacters and push to whichever
    // has an audio player attached. Multi-character LK rooms would route by ActiveVoiceCharacterId.
    int CharCount = 0;
    int CharsWithPlayer = 0;
    for (TActorIterator<AEstuaryCharacter> It(World); It; ++It)
    {
        AEstuaryCharacter* Character = *It;
        if (!IsValid(Character)) { continue; }
        ++CharCount;
        if (UEstuaryAudioPlayerComponent* Player = Character->FindComponentByClass<UEstuaryAudioPlayerComponent>())
        {
            ++CharsWithPlayer;
            if (bShouldLog)
            {
                LK_TRACE("RouteRemotePcmToPlayer #%d: bytes=%d SR=%d → enqueue to '%s'",
                    RouteNum, PcmBytes.Num(), SampleRate, *Character->GetName());
            }
            Player->EnqueueRawPcm16(PcmBytes, SampleRate);
            return;
        }
    }
    if (bShouldLog)
    {
        LK_TRACE("RouteRemotePcmToPlayer #%d: NO AEstuaryCharacter+player in world '%s' (chars=%d) — dropping %d bytes",
            RouteNum, *World->GetName(), CharCount, PcmBytes.Num());
    }
}

// ============================================================================
// Game-thread broadcast helpers
// ============================================================================

void UEstuaryLiveKitVoiceImpl::BroadcastJoined()
{
    check(IsInGameThread());
    OnJoined.Broadcast();
}

void UEstuaryLiveKitVoiceImpl::BroadcastLeft()
{
    check(IsInGameThread());
    OnLeft.Broadcast();
}

void UEstuaryLiveKitVoiceImpl::BroadcastError(const FString& Message)
{
    check(IsInGameThread());
    OnConnectionError.Broadcast(Message);
}

#else // !WITH_ESTUARY_LIVEKIT

// =============================================================================
// Stub mode — livekit/client-sdk-cpp is not vendored under ThirdParty/.
// =============================================================================

UEstuaryLiveKitVoiceImpl::UEstuaryLiveKitVoiceImpl() = default;
UEstuaryLiveKitVoiceImpl::~UEstuaryLiveKitVoiceImpl() = default;

void UEstuaryLiveKitVoiceImpl::BeginDestroy() { Super::BeginDestroy(); }

void UEstuaryLiveKitVoiceImpl::JoinRoom(const FString& /*Token*/, const FString& /*Url*/, const FString& /*Room*/)
{
    UE_LOG(LogEstuary, Warning,
        TEXT("[EstuaryLiveKit stub] JoinRoom called but livekit/client-sdk-cpp ")
        TEXT("is not vendored. Run ThirdParty/Setup.{ps1,sh} to fetch the prebuilt."));
}

void UEstuaryLiveKitVoiceImpl::LeaveRoom()       { /* no-op stub */ }
bool UEstuaryLiveKitVoiceImpl::IsJoined() const  { return false; }
void UEstuaryLiveKitVoiceImpl::SetMuted(bool)    { /* no-op stub */ }

void UEstuaryLiveKitVoiceImpl::PushMicPcm(const int16*, int32, int32) { /* no-op stub */ }
void UEstuaryLiveKitVoiceImpl::PushPlaybackPcm(const int16*, int32, int32) { /* no-op stub */ }
double UEstuaryLiveKitVoiceImpl::GetPlayoutClockSeconds() const { return 0.0; }

void UEstuaryLiveKitVoiceImpl::BroadcastJoined()                       { /* no-op stub */ }
void UEstuaryLiveKitVoiceImpl::BroadcastLeft()                         { /* no-op stub */ }
void UEstuaryLiveKitVoiceImpl::BroadcastError(const FString& /*Msg*/)  { /* no-op stub */ }

#endif // WITH_ESTUARY_LIVEKIT
