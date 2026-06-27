// EstuaryIOSAudioSession.h — iOS AVAudioSession category shim.
//
// Pitfall 8: The default UE audio session category on iOS is AVAudioSessionCategoryPlayback.
// This blocks the microphone. Switching to AVAudioSessionCategoryPlayAndRecord with
// AVAudioSessionCategoryOptionDefaultToSpeaker ensures TTS plays through the speaker
// while the microphone remains active — required for simultaneous voice + TTS.
//
// Usage:
//   Call Estuary::IOS::EnsureVoiceSession() before the first audio Play() on iOS.
//   Guarded by bIOSSessionActive in UEstuaryAudioPlayerComponent (T-04-04-03 mitigation).
//   Call Estuary::IOS::Teardown() when voice capture stops (optional — most apps keep
//   PlayAndRecord for the session duration).
//
// Platform behaviour:
//   iOS   : real implementation in EstuaryIOSAudioSession.mm (Objective-C++).
//   Others: inline no-ops — zero overhead, zero link cost.
//
// Pitfall 23: #pragma once, no CoreMinimal.h, IWYU.

#pragma once

namespace Estuary::IOS
{
#if PLATFORM_IOS
    // Declarations for iOS: implementations live in EstuaryIOSAudioSession.mm.

    /** Set the iOS AVAudioSession category to PlayAndRecord with DefaultToSpeaker + AllowBluetooth.
     *  Must be called on the game thread before the first audio Play().
     *  Safe to call multiple times — bIOSSessionActive in UEstuaryAudioPlayerComponent
     *  prevents redundant calls (T-04-04-03 mitigation).
     *  No-op on non-iOS platforms (inline below). */
    void EnsureVoiceSession();

    /** Optionally restore the session to a less permissive category when voice is inactive.
     *  Left as a no-op in the .mm for Phase 4 — Phase 5 (mic capture) may call this when
     *  stopping voice capture if the app needs to yield the mic. */
    void Teardown();

#else
    // Non-iOS platforms: inline no-ops — zero overhead, zero link cost.

    /** No-op on non-iOS platforms. */
    inline void EnsureVoiceSession() {}

    /** No-op on non-iOS platforms. */
    inline void Teardown() {}

#endif  // PLATFORM_IOS
}  // namespace Estuary::IOS
