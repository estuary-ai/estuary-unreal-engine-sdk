// EstuaryPermissionsIOS.mm — iOS-specific permission acquisition via AVAudioSession / AVCaptureDevice.
// Compiled ONLY when PLATFORM_IOS is defined (UE build system handles .mm files).
// Pitfall 8: iOS audio session permission must be requested before AVAudioSession can record.
// T-05-03-D mitigation: #if PLATFORM_IOS guard ensures Obj-C++ code is only compiled for iOS.

#if PLATFORM_IOS

#include "EstuaryPermissions.h"
#include "EstuaryLog.h"
#include "Async/Async.h"  // AsyncTask

#import <AVFoundation/AVFoundation.h>

namespace Estuary
{
namespace Permissions
{

void RequestMicrophone(TFunction<void(bool)> Callback)
{
    // AVAudioSession.requestRecordPermission: fires on an arbitrary background thread.
    // Marshal to game thread before invoking Callback (MarshalToGameThread pattern, Pitfall 1).
    [[AVAudioSession sharedInstance] requestRecordPermission:^(BOOL granted)
    {
        const bool bGranted = (bool)granted;
        AsyncTask(ENamedThreads::GameThread,
            [bGranted, Callback = MoveTemp(Callback)]()
            {
                UE_LOG(LogEstuary, Log,
                    TEXT("EstuaryPermissions: iOS microphone permission = %s"),
                    bGranted ? TEXT("granted") : TEXT("denied"));
                Callback(bGranted);
            });
    }];
}

void RequestCamera(TFunction<void(bool)> Callback)
{
    // AVCaptureDevice.requestAccess for video (camera).
    // Fires on an arbitrary background thread — marshal to game thread.
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                             completionHandler:^(BOOL granted)
    {
        const bool bGranted = (bool)granted;
        AsyncTask(ENamedThreads::GameThread,
            [bGranted, Callback = MoveTemp(Callback)]()
            {
                UE_LOG(LogEstuary, Log,
                    TEXT("EstuaryPermissions: iOS camera permission = %s"),
                    bGranted ? TEXT("granted") : TEXT("denied"));
                Callback(bGranted);
            });
    }];
}

} // namespace Permissions
} // namespace Estuary

#endif // PLATFORM_IOS
