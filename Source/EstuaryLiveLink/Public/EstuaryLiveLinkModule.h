#pragma once
// EstuaryLiveLinkModule.h — Live Link source registration module.
// Mirrors the EstuaryLiveKitModule shape for symmetry; differs in lifecycle:
//   - LiveLink is engine-bundled, NOT optional/third-party. No DLL load.
//   - The publisher is non-UObject (TSharedPtr<ILiveLinkSource>), not a UObject.
//   - ILiveLinkClient acquired via IModularFeatures (with deferred OnRegister
//     fallback if the LiveLinkInterface module loads after us).
//
// Path B — Live Link publishing pipeline.
// Pitfall 23: IWYU, no CoreMinimal.h.

#include "Modules/ModuleInterface.h"
#include "Engine/World.h"
#include "Delegates/IDelegateInstance.h"
#include "Templates/SharedPointer.h"

class FEstuaryLiveLinkPublisher;       // fwd decl, full include in .cpp
class FEstuaryBodyLiveLinkPublisher;   // fwd decl, full include in .cpp (Phase 4 UE-02)
class IModularFeature;
class ILiveLinkClient;

class ESTUARYLIVELINK_API FEstuaryLiveLinkModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    /** The publisher is non-UObject (ILiveLinkSource is non-UObject).
     *  TSharedPtr lifetime: created in TryRegisterWithLiveLinkClient(); ILiveLinkClient
     *  holds the matching shared ref via AddSource; we Reset() in ShutdownModule. */
    TSharedPtr<FEstuaryLiveLinkPublisher> Publisher;

    /** Body publisher — Phase 4 UE-02 (service-locator extension; Pitfall 24 mirror).
     *  Same TSharedPtr lifetime pattern as the face publisher; both are created in
     *  TryRegisterWithLiveLinkClient(), AddSource'd to the same ILiveLinkClient,
     *  and registered with the bridge in OnPostWorldInit. Same FeatureRegHandle covers
     *  both publishers — no second handle needed. */
    TSharedPtr<FEstuaryBodyLiveLinkPublisher> BodyPublisher;

    /** Cached Client* from IModularFeatures::GetModularFeature<ILiveLinkClient>.
     *  Used in ShutdownModule to call RemoveSource(SourceGuid) cleanly before
     *  dropping the publisher's TSharedPtr. */
    ILiveLinkClient* Client = nullptr;

    /** World init delegate — mirrors EstuaryLiveKitModule. Defer bridge registration
     *  to first PIE/Game world (when GameInstance + UEstuarySubsystem exist). */
    FDelegateHandle WorldInitHandle;
    void OnPostWorldInit(UWorld* World, const UWorld::InitializationValues IVS);

    /** Modular-feature deferral: LiveLink module may load AFTER this module at
     *  LoadingPhase=Default. If GetModularFeature would assert, we subscribe to
     *  OnModularFeatureRegistered and try again when LiveLink lands. */
    FDelegateHandle FeatureRegHandle;
    void OnModularFeatureRegistered(const FName& Type, IModularFeature* Feature);

    /** Helper that does the actual ILiveLinkClient::AddSource wiring once the feature
     *  is available. Idempotent — guards against double-register. Returns true on
     *  successful registration. */
    bool TryRegisterWithLiveLinkClient();
};
