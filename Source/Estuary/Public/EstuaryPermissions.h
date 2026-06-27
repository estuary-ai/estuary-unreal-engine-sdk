// EstuaryPermissions.h — Cross-platform runtime permission helpers.
// Pitfall 23: #pragma once, IWYU, no CoreMinimal.h.
//
// Platform routing:
//   iOS:     EstuaryPermissionsIOS.mm (Obj-C++ AVAudioSession / AVCaptureDevice)
//   Android: EstuaryPermissions.cpp   (UAndroidPermissionFunctionLibrary)
//   Desktop: EstuaryPermissions.cpp   (immediate grant, no dialog)

#pragma once

#include "CoreTypes.h"
#include "Templates/Function.h"

namespace Estuary
{
namespace Permissions
{

/**
 * Request the microphone permission at runtime.
 *
 * - iOS: Shows OS dialog via AVAudioSession.requestRecordPermission: (Obj-C++ .mm).
 *        Callback fires on the game thread after the user responds.
 * - Android: Shows OS dialog via UAndroidPermissionFunctionLibrary.
 *            Callback fires on the game thread after the user responds.
 * - Desktop (Win64, Mac): Microphone access does not require runtime permission.
 *            Callback fires immediately with true (wrapped in AsyncTask for consistency).
 *
 * @param Callback  Called with true if permission is granted, false if denied.
 *                  Always fires exactly once, always on the game thread.
 */
void RequestMicrophone(TFunction<void(bool bGranted)> Callback);

/**
 * Request the camera permission at runtime.
 * Same platform routing as RequestMicrophone but for camera access.
 * Included here for Phase 7 (UEstuaryCameraComponent) reuse.
 *
 * @param Callback  Called with true if permission is granted, false if denied.
 *                  Always fires exactly once, always on the game thread.
 */
void RequestCamera(TFunction<void(bool bGranted)> Callback);

} // namespace Permissions
} // namespace Estuary
