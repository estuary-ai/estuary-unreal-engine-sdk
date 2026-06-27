// EstuaryPermissionAction.h — Blueprint latent permission request nodes.
// Pitfall 23: #pragma once, IWYU, no CoreMinimal.h.
// Pitfall 2: Output pin delegates must be DYNAMIC_MULTICAST.

#pragma once

#include "CoreTypes.h"
#include "UObject/ObjectMacros.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Containers/UnrealString.h"

#include "EstuaryPermissionAction.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnEstuaryPermissionGranted);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnEstuaryPermissionDenied);

/**
 * Latent Blueprint node for runtime permission acquisition.
 *
 * Usage (Blueprint):
 *   "Acquire Microphone Permission" latent node -> Granted / Denied output pins
 *   "Acquire Camera Permission"     latent node -> Granted / Denied output pins
 *
 * Platform behavior:
 *   iOS:     Shows OS dialog. Callback on game thread after user responds.
 *   Android: Shows OS dialog. Callback on game thread after user responds.
 *   Desktop: Fires Granted immediately (no dialog needed).
 *
 * Memory safety: RegisterWithGameInstance() prevents GC during the async operation.
 *                SetReadyToDestroy() unregisters on completion (T-05-03-C mitigation).
 */
UCLASS()
class ESTUARY_API UEstuaryPermissionAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:

    /**
     * Request microphone permission. Fires Granted or Denied when the OS resolves.
     * On desktop, Granted fires immediately (wrapped in AsyncTask for consistency).
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Permissions",
              meta = (BlueprintInternalUseOnly = "true",
                      WorldContext = "WorldContextObject",
                      DisplayName = "Acquire Microphone Permission",
                      ToolTip = "Request OS microphone permission. Fires Granted or Denied output pin."))
    static UEstuaryPermissionAction* AcquireMicrophonePermission(UObject* WorldContextObject);

    /**
     * Request camera permission. Fires Granted or Denied when the OS resolves.
     * On desktop, Granted fires immediately. Included for Phase 7 (UEstuaryCameraComponent) use.
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Permissions",
              meta = (BlueprintInternalUseOnly = "true",
                      WorldContext = "WorldContextObject",
                      DisplayName = "Acquire Camera Permission",
                      ToolTip = "Request OS camera permission. Fires Granted or Denied output pin."))
    static UEstuaryPermissionAction* AcquireCameraPermission(UObject* WorldContextObject);

    virtual void Activate() override;

    /** Fires when the OS grants permission. */
    UPROPERTY(BlueprintAssignable, meta = (DisplayName = "Granted"))
    FOnEstuaryPermissionGranted OnGranted;

    /** Fires when the OS denies permission (or when the user cancels). */
    UPROPERTY(BlueprintAssignable, meta = (DisplayName = "Denied"))
    FOnEstuaryPermissionDenied OnDenied;

private:

    enum class EPermissionType : uint8
    {
        Microphone,
        Camera
    };

    EPermissionType PermissionType = EPermissionType::Microphone;
};
