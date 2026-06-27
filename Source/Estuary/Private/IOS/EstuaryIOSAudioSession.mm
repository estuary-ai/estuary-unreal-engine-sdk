// EstuaryIOSAudioSession.mm — iOS Objective-C++ implementation.
//
// File extension .mm causes Clang to treat this as Objective-C++.
// UE's build system automatically includes .mm files from source directories
// on iOS targets; no explicit registration in Build.cs is needed.
//
// Pitfall 8: AVAudioSessionCategoryPlayAndRecord + DefaultToSpeaker is required
//            for simultaneous TTS playback while the microphone is active on iOS.
//            The default UE session category (Playback) blocks the mic.
//
// Security note (T-04-04-02): PlayAndRecord is the standard iOS VOIP pattern.
// Apple requires NSMicrophoneUsageDescription in Info.plist (Phase 1 scaffolding).
// No privilege escalation beyond what mic access already grants.

#if PLATFORM_IOS

#import <AVFoundation/AVFoundation.h>
#include "EstuaryIOSAudioSession.h"
#include "EstuaryLog.h"

void Estuary::IOS::EnsureVoiceSession()
{
    @autoreleasepool
    {
        AVAudioSession* Session = [AVAudioSession sharedInstance];
        NSError* Error = nil;

        // Set category to PlayAndRecord with DefaultToSpeaker + AllowBluetooth.
        //   AVAudioSessionCategoryOptionDefaultToSpeaker : routes output to loud speaker,
        //                                                  not the ear receiver.
        //   AVAudioSessionCategoryOptionAllowBluetooth   : enables BT headsets for
        //                                                  both mic input and playback.
        BOOL Success = [Session
            setCategory:AVAudioSessionCategoryPlayAndRecord
            withOptions:(AVAudioSessionCategoryOptionDefaultToSpeaker |
                         AVAudioSessionCategoryOptionAllowBluetooth)
            error:&Error];

        if (!Success || Error != nil)
        {
            UE_LOG(LogEstuary, Warning,
                TEXT("EstuaryIOSAudioSession: failed to set PlayAndRecord category: %s"),
                Error ? *FString([Error localizedDescription]) : TEXT("unknown error"));
            return;
        }

        // Activate the session so the category change takes effect immediately.
        Error = nil;
        Success = [Session setActive:YES error:&Error];

        if (!Success || Error != nil)
        {
            UE_LOG(LogEstuary, Warning,
                TEXT("EstuaryIOSAudioSession: failed to activate session: %s"),
                Error ? *FString([Error localizedDescription]) : TEXT("unknown error"));
        }
        else
        {
            UE_LOG(LogEstuary, Log,
                TEXT("EstuaryIOSAudioSession: AVAudioSessionCategoryPlayAndRecord activated "
                     "(DefaultToSpeaker | AllowBluetooth)"));
        }
    }
}

void Estuary::IOS::Teardown()
{
    // Optional: restore to a less permissive category when voice is inactive.
    // Left as a no-op for Phase 4 — most AI chat apps keep PlayAndRecord
    // for the full session duration. Phase 5 (mic capture) may call this
    // when stopping voice capture if the app needs to yield the mic resource.
    //
    // To restore: [AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryPlayback
    //             error:nil] + setActive:YES.
}

#endif  // PLATFORM_IOS
