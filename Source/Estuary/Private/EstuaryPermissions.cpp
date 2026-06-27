// EstuaryPermissions.cpp — Desktop (Win64, Mac) and Android permission implementations.
// iOS is implemented in EstuaryPermissionsIOS.mm via #if PLATFORM_IOS guard.
//
// Pitfall 14: Android UPL declares the manifest permissions; this file handles runtime dialogs.
// Threading: Android callback may arrive off the game thread — AsyncTask marshals it back.

#include "EstuaryPermissions.h"
#include "EstuaryLog.h"
#include "Async/Async.h"  // AsyncTask

#if PLATFORM_ANDROID
#include "AndroidPermissionFunctionLibrary.h"
#include "AndroidPermissionCallbackProxy.h"
#endif

namespace Estuary
{
namespace Permissions
{

// iOS is handled entirely in EstuaryPermissionsIOS.mm.
// The #if !PLATFORM_IOS guard prevents a duplicate-symbol link error when both
// this .cpp and the .mm are compiled (Mac compiles .mm files but PLATFORM_IOS is false there).
#if !PLATFORM_IOS

void RequestMicrophone(TFunction<void(bool)> Callback)
{
#if PLATFORM_ANDROID
    // Android: show the runtime permission dialog. The delegate fires on an arbitrary thread
    // (JNI callback), so we marshal back to the game thread via AsyncTask.
    UAndroidPermissionFunctionLibrary::AcquirePermissions(
        { TEXT("android.permission.RECORD_AUDIO") },
        FAndroidPermissionDelegate::CreateLambda(
            [Callback = MoveTemp(Callback)](
                const TArray<FString>& /*Perms*/, const TArray<bool>& Results)
            {
                const bool bGranted = Results.Num() > 0 && Results[0];
                AsyncTask(ENamedThreads::GameThread,
                    [bGranted, Callback = MoveTemp(Callback)]()
                    {
                        Callback(bGranted);
                    });
            })
    );
#else
    // Desktop (Win64, Mac): no OS dialog required — grant immediately.
    // Wrap in AsyncTask for consistent async behaviour across platforms (callers
    // should not assume synchronous invocation even on desktop).
    UE_LOG(LogEstuary, Log,
        TEXT("EstuaryPermissions: microphone permission auto-granted on desktop"));
    AsyncTask(ENamedThreads::GameThread,
        [Callback = MoveTemp(Callback)]()
        {
            Callback(true);
        });
#endif
}

void RequestCamera(TFunction<void(bool)> Callback)
{
#if PLATFORM_ANDROID
    UAndroidPermissionFunctionLibrary::AcquirePermissions(
        { TEXT("android.permission.CAMERA") },
        FAndroidPermissionDelegate::CreateLambda(
            [Callback = MoveTemp(Callback)](
                const TArray<FString>& /*Perms*/, const TArray<bool>& Results)
            {
                const bool bGranted = Results.Num() > 0 && Results[0];
                AsyncTask(ENamedThreads::GameThread,
                    [bGranted, Callback = MoveTemp(Callback)]()
                    {
                        Callback(bGranted);
                    });
            })
    );
#else
    // Desktop (Win64, Mac): no OS dialog required.
    UE_LOG(LogEstuary, Log,
        TEXT("EstuaryPermissions: camera permission auto-granted on desktop"));
    AsyncTask(ENamedThreads::GameThread,
        [Callback = MoveTemp(Callback)]()
        {
            Callback(true);
        });
#endif
}

#endif // !PLATFORM_IOS

} // namespace Permissions
} // namespace Estuary
