// EstuaryBodyLiveLinkPublisher.h — Live Link source for GestureSync bot_pose body animation (UE-02).
//
// Mirror of FEstuaryLiveLinkPublisher (face) with role swap: ULiveLinkAnimationRole
// with FULL BoneNames + BoneParents + Transforms (vs. face's empty-bones hack —
// Pitfall 4). Publishes 52 transforms per tick (22 body + 30 fingers, MetaHuman
// skeleton order, post-remap).
//
// Lifecycle: registered by FEstuaryLiveLinkModule alongside the face publisher in
// StartupModule (Pitfall 24 mirror — same OnPostWorldInit + OnModularFeatureRegistered
// fallback as face). Subject naming: "<characterId>_Body" (suffix-on-body-only).
//
// Threading: PushSubject{Static,Frame}Data_AnyThread are internally locked by
// Live Link, so we call directly from the receiver's game-thread tick — no
// MarshalToGameThread shim. Our own SubjectsLock guards the per-subject schema
// map (FBodySubjectState) against concurrent Ensure/Publish from multiple subjects.
//
// Pitfall 7: lives ONLY in EstuaryLiveLink module; the IEstuaryBodyLiveLinkPublisher
// interface lives in core (Source/Estuary/Public/EstuaryLiveLinkBridge.h). Core
// has ZERO LiveLink/* includes.
// Pitfall 23: #pragma once, IWYU only, no CoreMinimal.h.
//
// Phase 4 — UE-02.

#pragma once

#include "ILiveLinkSource.h"
#include "LiveLinkTypes.h"
#include "Templates/SharedPointer.h"
#include "Containers/Map.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "UObject/NameTypes.h"
#include "Internationalization/Text.h"
#include "HAL/CriticalSection.h"
#include "Math/Transform.h"
#include "EstuaryLiveLinkBridge.h"   // IEstuaryBodyLiveLinkPublisher (plain C++)

class ILiveLinkClient;

/**
 * FEstuaryBodyLiveLinkPublisher — Live Link source for GestureSync 52-bone body+finger animation.
 *
 * Mirror of FEstuaryLiveLinkPublisher (face) with role swap: ULiveLinkAnimationRole
 * (transforms), NOT ULiveLinkBasicRole (curves), with FULL BoneNames + BoneParents +
 * Transforms (vs. face's empty-bones hack — Pitfall 4 mitigation; the body
 * success criterion).
 *
 * Inheritance:
 *   - ILiveLinkSource              — engine contract (six required overrides).
 *   - IEstuaryBodyLiveLinkPublisher — plain C++ Estuary interface (EnsureSubject /
 *                                     PublishFrame). Plain C++ because UINTERFACE
 *                                     would force a UObject impl, incompatible with
 *                                     TSharedFromThis<>.
 *   - TSharedFromThis<>             — required by ILiveLinkClient::AddSource.
 *
 * Lifetime: owned by FEstuaryLiveLinkModule via TSharedPtr. ILiveLinkClient
 * holds a matching TSharedPtr until RequestSourceShutdown returns true.
 *
 * Subject naming: "<characterId>_Body" (suffix-on-body-only). Face stays
 * bare-id to avoid breaking v1.0 face ABPs. The receiver enforces the
 * suffix at a single resolution helper.
 *
 * Pitfall 7: lives ONLY in EstuaryLiveLink module; the IEstuaryBodyLiveLinkPublisher
 * interface lives in core. Core has ZERO LiveLink/* includes.
 */
class ESTUARYLIVELINK_API FEstuaryBodyLiveLinkPublisher
    : public ILiveLinkSource
    , public IEstuaryBodyLiveLinkPublisher    // plain C++; safe with TSharedFromThis<>
    , public TSharedFromThis<FEstuaryBodyLiveLinkPublisher>
{
public:
    // ILiveLinkSource overrides (six required, mirror face structure verbatim
    // except for the source-type FText: "Estuary Body" disambiguates from face's
    // "Estuary" in the Live Link panel Sources tab).
    virtual void  ReceiveClient(class ILiveLinkClient* InClient, FGuid InSourceGuid) override;
    virtual bool  IsSourceStillValid() const override   { return Client != nullptr; }
    virtual bool  RequestSourceShutdown() override;
    virtual FText GetSourceType() const override        { return INVTEXT("Estuary Body"); }
    virtual FText GetSourceMachineName() const override { return INVTEXT("Estuary SDK"); }
    virtual FText GetSourceStatus() const override      { return INVTEXT("Active"); }

    // IEstuaryBodyLiveLinkPublisher overrides.
    virtual void EnsureSubject(const FName& SubjectName,
                               const TArray<FName>& BoneNames,
                               const TArray<int32>& BoneParents) override;

    virtual void PublishFrame(const FName& SubjectName,
                              const TArray<FTransform>& Transforms,
                              double WorldTime,
                              const FString& MessageId) override;

    /** Accessor for module shutdown — used to call Client->RemoveSource(SourceGuid).
     *  Mirror of face publisher's GetSourceGuid(). */
    FGuid GetSourceGuid() const { return SourceGuid; }

private:
    /** Per-subject schema state. Body shape: BoneNames + BoneParents (post-remap
     *  MetaHuman names + parent indices) instead of face's PropertyNames + curve
     *  names. The publisher does NO remap itself — the receiver pre-converts
     *  SMPL-X→UE quaternions and pre-resolves SMPL-X→MetaHuman bone names BEFORE
     *  calling EnsureSubject. Pitfall 7: keep UDataTable* access in core only. */
    struct FBodySubjectState
    {
        TArray<FName>  BoneNames;            // 52 FNames in MetaHuman skeleton order (post-remap)
        TArray<int32>  BoneParents;          // 52 parent indices (-1 == root); count must match BoneNames
        bool           bPushedStaticData = false;
        double         LastPublishedWorldTime = 0.0;  // monotonic guard — Live Link rejects
                                                       // duplicate-time frames ("subject is invalid"
                                                       // warning); we bump by 1e-6 if needed.
    };

    /** Build + push static data for a subject (idempotent via bPushedStaticData flag).
     *  Uses ULiveLinkAnimationRole::StaticClass() (transforms), NOT ULiveLinkBasicRole
     *  (curves) — required by the body success criterion. */
    void PushStaticDataForSubject(const FName& SubjectName, FBodySubjectState& State);

    class ILiveLinkClient*         Client = nullptr;
    FGuid                          SourceGuid;
    TMap<FName, FBodySubjectState> Subjects;
    FCriticalSection               SubjectsLock;
};
