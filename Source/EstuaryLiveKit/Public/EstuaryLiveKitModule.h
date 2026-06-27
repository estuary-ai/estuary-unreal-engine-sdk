#pragma once
// EstuaryLiveKitModule.h — Optional WebRTC wrapper module interface.
// Only compiled when EstuaryLiveKit is listed in the .uplugin and present.
// Phase 8: IEstuaryLiveKitVoice + IEstuaryLiveKitVideo implementations added.

#include "Modules/ModuleInterface.h"
#include "Engine/World.h"
#include "Delegates/IDelegateInstance.h"
#include "UObject/WeakObjectPtr.h"   // TWeakObjectPtr

// Forward-declare so the header compiles without EstuaryLiveKitVoiceImpl.h.
// The full include lives in EstuaryLiveKitModule.cpp inside #if WITH_ESTUARY_LIVEKIT.
class UEstuaryLiveKitVoiceImpl;

class ESTUARYLIVEKIT_API FEstuaryLiveKitModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    // -----------------------------------------------------------------------
    // VoiceImpl — owned by the module via AddToRoot/RemoveFromRoot.
    // Only present when WITH_ESTUARY_LIVEKIT=1.
    // TWeakObjectPtr (not a raw ptr): if the UObject is destroyed out from under us
    // — GC exit-purge on editor close, or any mid-session collect — .Get() returns
    // null instead of a dangling pointer, so ShutdownModule can never deref freed
    // memory (the EXCEPTION_ACCESS_VIOLATION-in-CoreUObject crash). AddToRoot still
    // keeps it alive during normal operation; RemoveFromRoot in ShutdownModule.
    // -----------------------------------------------------------------------
#if WITH_ESTUARY_LIVEKIT
    TWeakObjectPtr<UEstuaryLiveKitVoiceImpl> VoiceImpl;

    // Explicit DLL handles for the delay-loaded LiveKit prebuilts.
    // Without these, Windows' delay-load helper fails at first livekit:: call
    // because Plugins/<n>/ThirdParty/.../bin is not on the default search path.
    void* LkFfiDllHandle = nullptr;
    void* LkDllHandle = nullptr;

    // World init delegate — registers the voice impl with the EstuarySubsystem
    // bridge when a Game/PIE world becomes live. StartupModule is too early
    // because GEngine isn't ready and no GameInstance exists yet.
    FDelegateHandle WorldInitHandle;
    void OnPostWorldInit(UWorld* World, const UWorld::InitializationValues IVS);
#endif
};
