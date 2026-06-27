// EstuaryLiveLinkPublisher.h — Live Link source publisher (Task 2 implementation).
// Pushes ARKit-52 weights as ULiveLinkBasicRole curves via PushSubjectFrameData_AnyThread.
// Static data (52 PropertyNames per subject) pushed once per (subject, schema) pair.
// ARKit→target-rig remap performed inside this class (Option A from RESEARCH).
//
// Path B — Live Link publishing pipeline.
// Pitfall 23: IWYU, no CoreMinimal.h.

#pragma once

#include "ILiveLinkSource.h"
#include "LiveLinkTypes.h"
#include "Roles/LiveLinkBasicRole.h"
#include "Templates/SharedPointer.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "UObject/NameTypes.h"
#include "Internationalization/Text.h"
#include "HAL/CriticalSection.h"
#include "EstuaryLiveLinkBridge.h"   // IEstuaryLiveLinkPublisher (plain C++)

class UDataTable;
class ILiveLinkClient;

/**
 * FEstuaryLiveLinkPublisher — concrete Live Link source publishing ARKit-52 curves.
 *
 * Concrete ILiveLinkSource that consumes ARKit-52 weights from
 * UEstuaryAnimationReceiver and pushes them to the Live Link subsystem via
 * ULiveLinkBasicRole. Lives entirely in the EstuaryLiveLink module — core
 * Estuary code only sees the IEstuaryLiveLinkPublisher plain C++ interface.
 *
 * Inheritance:
 *   - ILiveLinkSource         — engine contract (six required overrides).
 *   - IEstuaryLiveLinkPublisher — plain C++ Estuary interface (EnsureSubject /
 *                                 PublishFrame). Plain C++ because UINTERFACE
 *                                 would force a UObject impl, incompatible with
 *                                 TSharedFromThis<>.
 *   - TSharedFromThis<>        — required by ILiveLinkClient::AddSource.
 *
 * Lifetime: owned by FEstuaryLiveLinkModule via TSharedPtr. ILiveLinkClient
 * holds a matching TSharedPtr until RequestSourceShutdown returns true.
 *
 * Threading: PushSubject{Static,Frame}Data_AnyThread are internally locked by
 * Live Link, so we call directly from the receiver's game-thread tick — no
 * MarshalToGameThread shim. Our own SubjectsLock guards the per-subject schema
 * map (FSubjectState) against concurrent Ensure/Publish from multiple subjects.
 */
class ESTUARYLIVELINK_API FEstuaryLiveLinkPublisher
    : public ILiveLinkSource
    , public IEstuaryLiveLinkPublisher   // plain C++; safe with TSharedFromThis<>
    , public TSharedFromThis<FEstuaryLiveLinkPublisher>
{
public:
    // ILiveLinkSource overrides (six required).
    virtual void  ReceiveClient(class ILiveLinkClient* InClient, FGuid InSourceGuid) override;
    virtual bool  IsSourceStillValid() const override   { return Client != nullptr; }
    virtual bool  RequestSourceShutdown() override;
    virtual FText GetSourceType() const override        { return INVTEXT("Estuary"); }
    virtual FText GetSourceMachineName() const override { return INVTEXT("Estuary SDK"); }
    virtual FText GetSourceStatus() const override      { return INVTEXT("Active"); }

    // IEstuaryLiveLinkPublisher overrides.
    virtual void EnsureSubject(const FName& SubjectName,
                               const UDataTable* RemapAsset) override;
    virtual void PublishFrame(const FName& SubjectName,
                              const TMap<FString, float>& InterpolatedWeights,
                              double WorldTime,
                              const FString& MessageId) override;

    /** Accessor for module shutdown — used to call Client->RemoveSource(SourceGuid). */
    FGuid GetSourceGuid() const { return SourceGuid; }

private:
    /** Per-subject schema state; built by EnsureSubject from RemapAsset on first frame
     *  (or on RemapAsset pointer change). PropertyNames are post-remap names that the
     *  LiveLinkPose AnimGraph node writes into Output.Curve. SourceWireNames are the
     *  pre-remap ARKit-52 names used to look up values from the receiver's TMap. */
    struct FSubjectState
    {
        TArray<FName>     PropertyNames;       // post-remap, paired by index
        TArray<FString>   SourceWireNames;     // wire-side name (TMap<FString,float> key)
        TArray<float>     ScaleFactors;        // 1.0 if no remap row, paired by index
        const UDataTable* RemapAsset = nullptr;
        bool              bPushedStaticData = false;
        double            LastPublishedWorldTime = 0.0;  // monotonic guard — Live Link
                                                          // rejects duplicate-time frames
                                                          // ("subject is invalid" warning).
    };

    class ILiveLinkClient*     Client = nullptr;
    FGuid                      SourceGuid;
    TMap<FName, FSubjectState> Subjects;
    FCriticalSection           SubjectsLock;

    // Internal helpers.
    void BuildSubjectSchema(FSubjectState& OutState, const UDataTable* RemapAsset) const;
    void PushStaticDataForSubject(const FName& SubjectName, FSubjectState& State);
};
