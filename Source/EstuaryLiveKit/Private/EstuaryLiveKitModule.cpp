// EstuaryLiveKitModule.cpp — Optional WebRTC wrapper module entry point.
// Compiles in both WITH_ESTUARY_LIVEKIT=1 and =0 configurations.
//
// Pitfall 7:  Core Estuary module never includes EstuaryLiveKit headers.
//             Registration flows the other direction: this module reaches INTO the
//             subsystem via GEngine to call GetLiveKitBridge().RegisterVoiceImpl().
// Pitfall 23: IWYU. No CoreMinimal.h.

#include "EstuaryLiveKitModule.h"
#include "EstuaryLog.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

#if WITH_ESTUARY_LIVEKIT
#include "EstuaryLiveKitVoiceImpl.h"
#pragma push_macro("check")
#pragma push_macro("PI")
#undef check
#undef PI
#include "livekit/livekit.h"
#pragma pop_macro("PI")
#pragma pop_macro("check")
#endif

#include "Engine/GameInstance.h"
#include "Engine/Engine.h"
#include "EstuarySubsystem.h"
#include "UObject/UObjectBase.h"      // UObjectInitialized()
#include "CoreGlobals.h"              // IsEngineExitRequested()

// ============================================================================
// StartupModule
// ============================================================================

void FEstuaryLiveKitModule::StartupModule()
{
#if WITH_ESTUARY_LIVEKIT
    UE_LOG(LogEstuary, Log,
        TEXT("EstuaryLiveKit: StartupModule — livekit-client-sdk-cpp present, registering voice impl."));

    // Explicit DLL load before any livekit:: call so the delay-load stub resolves
    // against an already-loaded module instead of failing 0xc06d007e.
    {
        TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Estuary"));
        if (!Plugin.IsValid())
        {
            UE_LOG(LogEstuary, Error,
                TEXT("EstuaryLiveKit: IPluginManager could not find 'Estuary' plugin — aborting LiveKit init."));
            return;
        }

        const FString BinDir = FPaths::Combine(
            Plugin->GetBaseDir(), TEXT("ThirdParty"), TEXT("livekit-client-sdk-cpp"), TEXT("bin"));

#if PLATFORM_WINDOWS
        const FString FfiDllPath = FPaths::Combine(BinDir, TEXT("livekit_ffi.dll"));
        const FString LkDllPath  = FPaths::Combine(BinDir, TEXT("livekit.dll"));
#elif PLATFORM_MAC
        const FString FfiDllPath = FPaths::Combine(BinDir, TEXT("liblivekit_ffi.dylib"));
        const FString LkDllPath  = FPaths::Combine(BinDir, TEXT("liblivekit.dylib"));
#else
        const FString FfiDllPath = FPaths::Combine(BinDir, TEXT("liblivekit_ffi.so"));
        const FString LkDllPath  = FPaths::Combine(BinDir, TEXT("liblivekit.so"));
#endif

        // Load FFI first — livekit.dll depends on it.
        LkFfiDllHandle = FPlatformProcess::GetDllHandle(*FfiDllPath);
        if (!LkFfiDllHandle)
        {
            UE_LOG(LogEstuary, Error,
                TEXT("EstuaryLiveKit: Failed to load livekit_ffi DLL from '%s' — LiveKit voice unavailable."), *FfiDllPath);
            return;
        }

        LkDllHandle = FPlatformProcess::GetDllHandle(*LkDllPath);
        if (!LkDllHandle)
        {
            UE_LOG(LogEstuary, Error,
                TEXT("EstuaryLiveKit: Failed to load livekit DLL from '%s' — LiveKit voice unavailable."), *LkDllPath);
            FPlatformProcess::FreeDllHandle(LkFfiDllHandle);
            LkFfiDllHandle = nullptr;
            return;
        }

        UE_LOG(LogEstuary, Log,
            TEXT("EstuaryLiveKit: DLLs loaded explicitly from '%s'."), *BinDir);
    }

    // SDK global init — must run before first Room use, once per process.
    try
    {
        const bool bInitialized = livekit::initialize();
        UE_LOG(LogEstuary, Log,
            TEXT("EstuaryLiveKit: livekit::initialize() returned %s"),
            bInitialized ? TEXT("true (newly initialised)") : TEXT("false (already initialised)"));
    }
    catch (const std::exception& e)
    {
        UE_LOG(LogEstuary, Error,
            TEXT("EstuaryLiveKit: livekit::initialize() threw: %s"), UTF8_TO_TCHAR(e.what()));
        return;
    }

    // GEngine and GameInstance are not ready at LoadingPhase=Default. Defer the
    // bridge registration until a Game/PIE world initialises — that's when the
    // GameInstance subsystem exists and the bridge is reachable.
    WorldInitHandle = FWorldDelegates::OnPostWorldInitialization.AddRaw(
        this, &FEstuaryLiveKitModule::OnPostWorldInit);

    UE_LOG(LogEstuary, Log,
        TEXT("EstuaryLiveKit: Subscribed to OnPostWorldInitialization — voice impl will register on first PIE/Game world."));

#else
    UE_LOG(LogEstuary, Log,
        TEXT("EstuaryLiveKit: StartupModule — wrapper absent (ThirdParty/livekit-client-sdk-cpp not found). "
             "WebSocket voice fallback is active."));
#endif
}

// ============================================================================
// ShutdownModule
// ============================================================================

#if WITH_ESTUARY_LIVEKIT
void FEstuaryLiveKitModule::OnPostWorldInit(UWorld* World, const UWorld::InitializationValues IVS)
{
    if (!World)
    {
        return;
    }

    // Skip editor / preview / inactive worlds — only register for real game contexts.
    if (World->WorldType != EWorldType::Game && World->WorldType != EWorldType::PIE)
    {
        return;
    }

    UGameInstance* GameInstance = World->GetGameInstance();
    if (!GameInstance)
    {
        return;
    }

    UEstuarySubsystem* Subsystem = GameInstance->GetSubsystem<UEstuarySubsystem>();
    if (!Subsystem)
    {
        return;
    }

    // Reuse the voice impl across PIE sessions; the bridge itself may be a per-
    // GameInstance instance, so re-register on every world init.
    UEstuaryLiveKitVoiceImpl* Impl = VoiceImpl.Get();
    if (!Impl)
    {
        Impl = NewObject<UEstuaryLiveKitVoiceImpl>(
            GetTransientPackage(),
            UEstuaryLiveKitVoiceImpl::StaticClass());
        Impl->AddToRoot();
        VoiceImpl = Impl;
    }

    Subsystem->GetLiveKitBridge().RegisterVoiceImpl(Impl);

    UE_LOG(LogEstuary, Log,
        TEXT("EstuaryLiveKit: Voice impl registered with FEstuaryLiveKitBridge (WorldType=%d)."),
        static_cast<int32>(World->WorldType));
}
#endif

void FEstuaryLiveKitModule::ShutdownModule()
{
#if WITH_ESTUARY_LIVEKIT
    UE_LOG(LogEstuary, Log, TEXT("EstuaryLiveKit: ShutdownModule — unregistering voice impl."));

    if (WorldInitHandle.IsValid())
    {
        FWorldDelegates::OnPostWorldInitialization.Remove(WorldInitHandle);
        WorldInitHandle.Reset();
    }

    // CRITICAL: do NOT touch any UObject during engine/editor exit. On editor close the
    // GC exit-purge destroys every UObject first (the root set is ignored at exit) and
    // tears down GUObjectArray, THEN modules unload. Calling RemoveFromRoot() (or the
    // world/subsystem walk) at that point faults with EXCEPTION_ACCESS_VIOLATION reading
    // 0x0 inside CoreUObject. The exit guard is checked BEFORE VoiceImpl.Get() so we
    // never query the object array once it is torn down; the TWeakObjectPtr then yields
    // null (not a dangling ptr) if the impl was already collected for any other reason.
    if (UObjectInitialized() && !IsEngineExitRequested())
    {
        if (UEstuaryLiveKitVoiceImpl* Impl = VoiceImpl.Get())
        {
            // Unregister from the bridge FIRST so IsAvailable() returns false immediately.
            // Walk world contexts the same way as StartupModule.
            if (GEngine)
            {
                for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
                {
                    if (Ctx.World() && Ctx.World()->GetGameInstance())
                    {
                        if (UEstuarySubsystem* Subsystem =
                                Ctx.World()->GetGameInstance()->GetSubsystem<UEstuarySubsystem>())
                        {
                            Subsystem->GetLiveKitBridge().UnregisterVoiceImpl();
                            UE_LOG(LogEstuary, Log,
                                TEXT("EstuaryLiveKit: Voice impl unregistered from FEstuaryLiveKitBridge."));
                            break;
                        }
                    }
                }
            }

            // Remove the GC root so the UObject can be collected.
            Impl->RemoveFromRoot();
        }
    }

    VoiceImpl.Reset();

    try
    {
        livekit::shutdown();
        UE_LOG(LogEstuary, Log, TEXT("EstuaryLiveKit: livekit::shutdown() complete."));
    }
    catch (const std::exception& e)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("EstuaryLiveKit: livekit::shutdown() threw: %s"), UTF8_TO_TCHAR(e.what()));
    }

    // Reverse-order DLL release: livekit.dll first (depends on FFI), then FFI.
    if (LkDllHandle)
    {
        FPlatformProcess::FreeDllHandle(LkDllHandle);
        LkDllHandle = nullptr;
    }
    if (LkFfiDllHandle)
    {
        FPlatformProcess::FreeDllHandle(LkFfiDllHandle);
        LkFfiDllHandle = nullptr;
    }
#endif
}

IMPLEMENT_MODULE(FEstuaryLiveKitModule, EstuaryLiveKit)
