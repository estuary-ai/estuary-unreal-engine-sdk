// EstuaryPermissionAction.cpp — UEstuaryPermissionAction latent node implementation.
// Pitfall 3: TWeakObjectPtr guard prevents use-after-GC in the async callback.
// Pitfall 23: IWYU includes.

#include "EstuaryPermissionAction.h"
#include "EstuaryPermissions.h"
#include "EstuaryLog.h"

// ============================================================================
// Static factory constructors
// ============================================================================

UEstuaryPermissionAction* UEstuaryPermissionAction::AcquireMicrophonePermission(
    UObject* WorldContextObject)
{
    UEstuaryPermissionAction* Action = NewObject<UEstuaryPermissionAction>(WorldContextObject);
    Action->PermissionType = EPermissionType::Microphone;
    // RegisterWithGameInstance prevents GC during the latent async operation (T-05-03-C mitigation).
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

UEstuaryPermissionAction* UEstuaryPermissionAction::AcquireCameraPermission(
    UObject* WorldContextObject)
{
    UEstuaryPermissionAction* Action = NewObject<UEstuaryPermissionAction>(WorldContextObject);
    Action->PermissionType = EPermissionType::Camera;
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

// ============================================================================
// Activate
// ============================================================================

void UEstuaryPermissionAction::Activate()
{
    check(IsInGameThread());

    TWeakObjectPtr<UEstuaryPermissionAction> WeakThis(this);

    auto Callback = [WeakThis](bool bGranted)
    {
        // Callback guaranteed on game thread by all Estuary::Permissions implementations.
        if (UEstuaryPermissionAction* Action = WeakThis.Get())
        {
            if (bGranted)
            {
                Action->OnGranted.Broadcast();
            }
            else
            {
                Action->OnDenied.Broadcast();
            }
            // SetReadyToDestroy unregisters from GameInstance — prevents accumulation (T-05-03-C).
            Action->SetReadyToDestroy();
        }
    };

    if (PermissionType == EPermissionType::Microphone)
    {
        UE_LOG(LogEstuary, Log,
            TEXT("[EstuaryPermissionAction] Requesting microphone permission"));
        Estuary::Permissions::RequestMicrophone(MoveTemp(Callback));
    }
    else
    {
        UE_LOG(LogEstuary, Log,
            TEXT("[EstuaryPermissionAction] Requesting camera permission"));
        Estuary::Permissions::RequestCamera(MoveTemp(Callback));
    }
}
