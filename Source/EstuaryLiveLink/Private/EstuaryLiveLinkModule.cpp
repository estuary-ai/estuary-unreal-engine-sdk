// EstuaryLiveLinkModule.cpp — Live Link source registration module entry point.
//
// Pitfall 7:  Core Estuary module never includes EstuaryLiveLink headers.
//             Registration flows the other direction: this module reaches INTO the
//             subsystem via GEngine to call GetLiveLinkBridge().RegisterPublisher().
// Pitfall 23: IWYU. No CoreMinimal.h.
//
// Lifecycle:
//   1. StartupModule:
//      - Try to acquire ILiveLinkClient via IModularFeatures. If unavailable
//        (LiveLinkInterface module not yet loaded), defer via OnModularFeatureRegistered.
//      - Create the publisher TSharedPtr; AddSource on the LiveLink client.
//      - Subscribe to OnPostWorldInitialization for bridge registration.
//   2. OnPostWorldInit (Game/PIE world): find UEstuarySubsystem, call
//      GetLiveLinkBridge().RegisterPublisher(Publisher.Get()).
//   3. ShutdownModule: unhook delegates, unregister bridge, RemoveSource, drop publisher.

#include "EstuaryLiveLinkModule.h"
#include "EstuaryLiveLinkPublisher.h"
#include "EstuaryBodyLiveLinkPublisher.h"   // Phase 4 UE-02 — body publisher
#include "EstuaryLog.h"
#include "EstuarySubsystem.h"
#include "EstuaryLiveLinkBridge.h"

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

// ============================================================================
// StartupModule
// ============================================================================

void FEstuaryLiveLinkModule::StartupModule()
{
    UE_LOG(LogEstuary, Log, TEXT("EstuaryLiveLink: StartupModule"));

    // Try to register with the LiveLink modular feature now. If LiveLinkInterface
    // hasn't loaded yet (LoadingPhase=Default ordering is unspecified across
    // engine-bundled modules), subscribe to the registration delegate and try
    // again when LiveLink lands.
    if (!TryRegisterWithLiveLinkClient())
    {
        FeatureRegHandle = IModularFeatures::Get().OnModularFeatureRegistered().AddRaw(
            this, &FEstuaryLiveLinkModule::OnModularFeatureRegistered);
        UE_LOG(LogEstuary, Log,
            TEXT("EstuaryLiveLink: ILiveLinkClient not yet registered — deferring via OnModularFeatureRegistered."));
    }

    // Mirror EstuaryLiveKitModule: bridge registration happens once a Game/PIE
    // world initialises (GameInstance subsystem must exist to reach the bridge).
    WorldInitHandle = FWorldDelegates::OnPostWorldInitialization.AddRaw(
        this, &FEstuaryLiveLinkModule::OnPostWorldInit);
}

// ============================================================================
// TryRegisterWithLiveLinkClient
// ============================================================================

bool FEstuaryLiveLinkModule::TryRegisterWithLiveLinkClient()
{
    if (Publisher.IsValid())
    {
        // Already registered.
        return true;
    }

    IModularFeatures& Features = IModularFeatures::Get();
    if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
    {
        return false;
    }

    Client = &Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
    if (!Client)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("EstuaryLiveLink: GetModularFeature returned null despite IsModularFeatureAvailable==true."));
        return false;
    }

    Publisher = MakeShared<FEstuaryLiveLinkPublisher>();

    // AddSource(TSharedPtr<ILiveLinkSource>) calls Publisher->ReceiveClient(Client, NewGuid)
    // synchronously, which populates Publisher->SourceGuid for later RemoveSource.
    Client->AddSource(Publisher);

    UE_LOG(LogEstuary, Log,
        TEXT("EstuaryLiveLink: face publisher created and AddSource succeeded — guid=%s"),
        *Publisher->GetSourceGuid().ToString());

    // Phase 4 UE-02 (Pitfall 24 mirror) — body publisher: parallel to face,
    // registered in same lifecycle. Same ILiveLinkClient acquisition + AddSource +
    // OnModularFeatureRegistered fallback applies. Both publishers share this single
    // success path; the existing FeatureRegHandle covers both (no second handle needed).
    BodyPublisher = MakeShared<FEstuaryBodyLiveLinkPublisher>();
    Client->AddSource(BodyPublisher);

    UE_LOG(LogEstuary, Log,
        TEXT("EstuaryLiveLink: body publisher created and AddSource succeeded — guid=%s"),
        *BodyPublisher->GetSourceGuid().ToString());

    return true;
}

// ============================================================================
// OnModularFeatureRegistered — fires when any modular feature registers.
// ============================================================================

void FEstuaryLiveLinkModule::OnModularFeatureRegistered(const FName& Type, IModularFeature* /*Feature*/)
{
    if (Type != ILiveLinkClient::ModularFeatureName)
    {
        return;
    }

    if (TryRegisterWithLiveLinkClient())
    {
        // Unsubscribe — registration is one-shot.
        if (FeatureRegHandle.IsValid())
        {
            IModularFeatures::Get().OnModularFeatureRegistered().Remove(FeatureRegHandle);
            FeatureRegHandle.Reset();
        }
    }
}

// ============================================================================
// OnPostWorldInit — register publisher with the per-GameInstance bridge.
// ============================================================================

void FEstuaryLiveLinkModule::OnPostWorldInit(UWorld* World, const UWorld::InitializationValues /*IVS*/)
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

    if (!Publisher.IsValid())
    {
        // LiveLinkInterface still hasn't loaded — TryRegisterWithLiveLinkClient
        // will fire when OnModularFeatureRegistered eventually runs. The bridge
        // simply remains UnAvailable() until then; the receiver no-ops.
        UE_LOG(LogEstuary, Verbose,
            TEXT("EstuaryLiveLink: OnPostWorldInit fired before publisher was created — "
                 "bridge will register on next world init after LiveLink loads."));
        return;
    }

    // Pass the IEstuaryLiveLinkPublisher* via the bridge interface (publisher
    // derives from both ILiveLinkSource and IEstuaryLiveLinkPublisher).
    Subsystem->GetLiveLinkBridge().RegisterPublisher(
        static_cast<IEstuaryLiveLinkPublisher*>(Publisher.Get()));

    UE_LOG(LogEstuary, Log,
        TEXT("EstuaryLiveLink: face publisher registered with FEstuaryLiveLinkBridge (WorldType=%d)."),
        static_cast<int32>(World->WorldType));

    // Phase 4 UE-02 (service-locator extension) — register body publisher
    // alongside face. Body publisher derives from both ILiveLinkSource and
    // IEstuaryBodyLiveLinkPublisher; cast to the body interface for the bridge.
    if (BodyPublisher.IsValid())
    {
        Subsystem->GetLiveLinkBridge().RegisterBodyPublisher(
            static_cast<IEstuaryBodyLiveLinkPublisher*>(BodyPublisher.Get()));

        UE_LOG(LogEstuary, Log,
            TEXT("EstuaryLiveLink: body publisher registered with FEstuaryLiveLinkBridge (WorldType=%d)."),
            static_cast<int32>(World->WorldType));
    }
}

// ============================================================================
// ShutdownModule
// ============================================================================

void FEstuaryLiveLinkModule::ShutdownModule()
{
    UE_LOG(LogEstuary, Log, TEXT("EstuaryLiveLink: ShutdownModule"));

    // Unhook delegates first so no callback re-enters during teardown.
    if (WorldInitHandle.IsValid())
    {
        FWorldDelegates::OnPostWorldInitialization.Remove(WorldInitHandle);
        WorldInitHandle.Reset();
    }
    if (FeatureRegHandle.IsValid())
    {
        IModularFeatures::Get().OnModularFeatureRegistered().Remove(FeatureRegHandle);
        FeatureRegHandle.Reset();
    }

    // Unregister from every reachable bridge so IsAvailable() / IsBodyAvailable()
    // return false immediately. Walk world contexts the same way as
    // EstuaryLiveKitModule. Body and face are unregistered in the same loop pass
    // (service-locator parallel teardown).
    if (GEngine)
    {
        for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
        {
            if (Ctx.World() && Ctx.World()->GetGameInstance())
            {
                if (UEstuarySubsystem* Subsystem =
                        Ctx.World()->GetGameInstance()->GetSubsystem<UEstuarySubsystem>())
                {
                    Subsystem->GetLiveLinkBridge().UnregisterPublisher();
                    // Phase 4 UE-02 — parallel body teardown.
                    Subsystem->GetLiveLinkBridge().UnregisterBodyPublisher();
                }
            }
        }
    }

    // Remove our sources from Live Link before dropping the publisher TSharedPtrs.
    // Live Link calls each publisher's RequestSourceShutdown internally, which nulls
    // the Client* on the publisher. Body and face removal order is irrelevant for
    // correctness as long as both happen before the Client = nullptr line.
    if (Client && Publisher.IsValid())
    {
        Client->RemoveSource(Publisher->GetSourceGuid());
    }
    // Phase 4 UE-02 — parallel body source removal.
    if (Client && BodyPublisher.IsValid())
    {
        Client->RemoveSource(BodyPublisher->GetSourceGuid());
    }
    Client = nullptr;

    // Drop our shared refs. Live Link's matching refs were released in RemoveSource;
    // the publishers destruct here.
    Publisher.Reset();
    // Phase 4 UE-02 — parallel body Reset.
    BodyPublisher.Reset();
}

IMPLEMENT_MODULE(FEstuaryLiveLinkModule, EstuaryLiveLink)
