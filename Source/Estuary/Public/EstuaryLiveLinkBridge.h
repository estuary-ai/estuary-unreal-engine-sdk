// EstuaryLiveLinkBridge.h — IEstuaryLiveLinkPublisher (plain C++ interface) and
// FEstuaryLiveLinkBridge service-locator singleton.
//
// Lives in Source/Estuary/Public/ (core module). ZERO LiveLink/LiveLinkInterface
// includes — pure interface bridge (Pitfall 7 mirror of FEstuaryLiveKitBridge SHAPE).
//
// Structural deviation from FEstuaryLiveKitBridge: IEstuaryLiveLinkPublisher is a
// PLAIN C++ interface (NOT a UINTERFACE pair). The concrete impl
// FEstuaryLiveLinkPublisher is a non-UObject TSharedPtr<>-owned class deriving
// from ILiveLinkSource + TSharedFromThis<> (engine requirement of
// ILiveLinkClient::AddSource). UINTERFACE would force the implementer to be a
// UObject, which is incompatible with TSharedFromThis<> and the LiveLink source
// contract. Therefore: NO UINTERFACE, NO GENERATED_BODY(), NO UObject half, NO
// EstuaryLiveLinkBridge.generated.h include.
//
// Path B — Live Link publishing pipeline.
// Pitfall 23: #pragma once, IWYU, no CoreMinimal.h.

#pragma once

// IWYU-only includes — NO `EstuaryLiveLinkBridge.generated.h` (no UObject reflection here).
#include "Containers/UnrealString.h"
#include "Containers/Map.h"
#include "Containers/Array.h"           // TArray<FName>, TArray<int32>, TArray<FTransform>
#include "UObject/NameTypes.h"          // FName
#include "Math/Transform.h"             // FTransform (body publisher interface)
#include "Misc/AssertionMacros.h"       // check()
#include "HAL/Platform.h"

// Forward declarations only — no Engine/DataTable.h pull-in (avoids cyclic
// include exposure in core public headers; consumers include it themselves).
class UDataTable;

// ---------------------------------------------------------------------------
// IEstuaryLiveLinkPublisher — plain C++ publisher interface (NOT a UINTERFACE).
// Implemented in EstuaryLiveLink module by FEstuaryLiveLinkPublisher.
// Both methods are game-thread-callable; the impl uses
// PushSubjectFrameData_AnyThread internally so callers don't marshal.
// ---------------------------------------------------------------------------

/**
 * Plain C++ publisher interface — NOT a UINTERFACE.
 *
 * Concrete implementation: FEstuaryLiveLinkPublisher in the EstuaryLiveLink module.
 * Lives outside UObject reflection because Live Link's ILiveLinkSource requires a
 * non-UObject TSharedPtr<>-owned implementer (TSharedFromThis<...>); a UINTERFACE
 * would force a UObject impl, breaking compatibility with
 * ILiveLinkClient::AddSource(TSharedPtr<ILiveLinkSource>).
 *
 * Thread safety: both methods MUST be called on the game thread. The concrete impl
 * pushes to Live Link via PushSubjectStaticData_AnyThread / PushSubjectFrameData_AnyThread,
 * which are internally locked — no MarshalToGameThread shim needed at the call site.
 */
class ESTUARY_API IEstuaryLiveLinkPublisher
{
public:
    virtual ~IEstuaryLiveLinkPublisher() = default;

    /**
     * Build (or rebuild on RemapAsset change) the per-subject schema and push the
     * static data exactly once per (subject, schema) pair. Idempotent — safe to call
     * repeatedly with a stable RemapAsset; a defensive call with RemapAsset=nullptr
     * is a no-op when the subject already has established state.
     *
     * @param SubjectName  FName resolved by UEstuaryCharacterComponent::GetLiveLinkSubjectName().
     * @param RemapAsset   Optional ARKit→target-rig remap DataTable (DT_ArkitRemap rows);
     *                     nullptr selects identity remap. Set-once semantics per subject.
     */
    virtual void EnsureSubject(const FName& SubjectName,
                               const UDataTable* RemapAsset) = 0;

    /**
     * Push one frame's worth of ARKit-52 weights to the subject. Internally clamps each
     * value to [0,1] and applies the remap ScaleFactor captured at EnsureSubject time.
     * MetaData["estuary_message_id"] is populated for diagnostics; LiveLinkPose ignores it.
     *
     * @param SubjectName           Subject FName matching a prior EnsureSubject call.
     * @param InterpolatedWeights   Wire-side ARKit-52 names → 0..1 floats (size == 52,
     *                              but missing keys default to 0.0). Caller need not pre-clamp.
     * @param WorldTime             FApp::GetCurrentTime() seconds (monotonic; lipsync clock).
     * @param MessageId             Estuary message_id for diagnostic stamping; may be empty.
     */
    virtual void PublishFrame(const FName& SubjectName,
                              const TMap<FString, float>& InterpolatedWeights,
                              double WorldTime,
                              const FString& MessageId) = 0;
};

// ---------------------------------------------------------------------------
// IEstuaryBodyLiveLinkPublisher — plain C++ body publisher interface (NOT a UINTERFACE).
// Implemented in EstuaryLiveLink module by FEstuaryBodyLiveLinkPublisher.
// Parallel to IEstuaryLiveLinkPublisher (face); the only difference at the
// interface boundary is the data shape: ULiveLinkAnimationRole transforms
// (TArray<FTransform>) instead of ULiveLinkBasicRole curves (TMap<FString,float>).
// ZERO LiveLink/* includes here (Pitfall 7) — only plain C++ types.
// ---------------------------------------------------------------------------

/**
 * Plain C++ body publisher interface — NOT a UINTERFACE.
 *
 * Concrete implementation: FEstuaryBodyLiveLinkPublisher in the EstuaryLiveLink module.
 * Same rationale as IEstuaryLiveLinkPublisher (face): Live Link's ILiveLinkSource
 * requires a non-UObject TSharedPtr<>-owned implementer; UINTERFACE would force a
 * UObject impl, breaking compatibility with TSharedFromThis<>.
 *
 * Lifecycle:
 *   1) UEstuaryBodyAnimationReceiver::TickComponent calls
 *      EnsureSubject(SubjectName, BoneNames, BoneParents) ONCE per (subject, schema)
 *      pair. Idempotent on repeat calls with same arguments.
 *   2) Per tick: receiver calls PublishFrame(SubjectName, Transforms, WorldTime, MessageId).
 *   3) Bridge registration / unregistration via FEstuaryLiveLinkBridge::Register/UnregisterBodyPublisher.
 *
 * The receiver is responsible for:
 *   - Pre-converting SMPL-X→UE quaternions via FEstuaryBodyFrameTransform::ToUEQuatFromGestureSync (UE-05)
 *   - DataTable name remap via UEstuaryBodyBoneRemapLibrary (UE-04)
 *   - Pre-allocating Transforms TArray to avoid per-tick allocation (Pitfall 6)
 * The publisher only sees POST-CONVERSION + POST-REMAP MetaHuman names + UE transforms.
 *
 * Thread safety: both methods MUST be called on the game thread. The concrete impl
 * pushes via PushSubjectStaticData_AnyThread / PushSubjectFrameData_AnyThread, which
 * are internally locked — no MarshalToGameThread shim needed at the call site.
 */
// NOTE: No ESTUARY_API on this pure interface. Pure virtual interfaces do not need
// DLL export — they have no instantiable symbols (no ctor/dtor body, no out-of-line
// methods). The concrete impl FEstuaryBodyLiveLinkPublisher in EstuaryLiveLink module
// emits its own vtable and references the subclass dtor inline. Adding ESTUARY_API
// here triggers MSVC to mark `IEstuaryBodyLiveLinkPublisher::~IEstuaryBodyLiveLinkPublisher`
// as dllimport, but no .cpp in the Estuary module materialises the symbol — yielding
// LNK2019 unresolved-external when the subclass's compiler-generated dtor calls the
// base dtor. Face's interface uses ESTUARY_API but only works because of historical
// linker-emit luck — the cleaner pattern is to omit it for both. (TODO: align face later.)
class IEstuaryBodyLiveLinkPublisher
{
public:
    virtual ~IEstuaryBodyLiveLinkPublisher() = default;

    /**
     * Build (or rebuild on schema change) the per-subject skeleton static data and
     * push it once per (subject, BoneNames) pair. Idempotent — safe to call every tick.
     *
     * @param SubjectName  e.g. "<characterId>_Body" (resolved by the receiver).
     * @param BoneNames    52 FNames in MetaHuman skeleton order (post-remap).
     * @param BoneParents  52 int32 parent indices (-1 == root); count must match BoneNames.
     */
    virtual void EnsureSubject(const FName& SubjectName,
                               const TArray<FName>& BoneNames,
                               const TArray<int32>& BoneParents) = 0;

    /**
     * Push one frame's 52 transforms (index 0 = hips with translation; rest pure rotation).
     *
     * @param SubjectName  Subject FName matching a prior EnsureSubject call.
     * @param Transforms   Paired by index with the BoneNames passed to EnsureSubject.
     *                     Counts must match — mismatch is logged and skipped.
     * @param WorldTime    FApp::GetCurrentTime() seconds; publisher guards against
     *                     duplicate-time frames internally (monotonic +1e-6 bump).
     * @param MessageId    Estuary message_id for diagnostic stamping; empty string skips.
     */
    virtual void PublishFrame(const FName& SubjectName,
                              const TArray<FTransform>& Transforms,
                              double WorldTime,
                              const FString& MessageId) = 0;
};

// ---------------------------------------------------------------------------
// FEstuaryLiveLinkBridge — service-locator singleton.
// ---------------------------------------------------------------------------

/**
 * Service-locator bridge for the optional Live Link publisher.
 *
 * The core Estuary module owns one of these as a value member inside UEstuarySubsystem.
 * The EstuaryLiveLink module registers a concrete IEstuaryLiveLinkPublisher* in
 * StartupModule (deferred to OnPostWorldInitialization for symmetry with EstuaryLiveKit).
 * Core code only ever calls through this bridge — never includes LiveLink headers.
 *
 * Usage (core code, e.g., UEstuaryAnimationReceiver::TickComponent):
 *   FEstuaryLiveLinkBridge& Bridge = Subsystem->GetLiveLinkBridge();
 *   if (Bridge.IsAvailable())
 *   {
 *       IEstuaryLiveLinkPublisher* Pub = Bridge.GetPublisher();
 *       if (Pub) { Pub->PublishFrame(SubjectName, Weights, WorldTime, MessageId); }
 *   }
 *
 * Thread safety: all methods MUST be called on the game thread.
 *
 * Lifetime: the bridge stores a plain raw pointer to the publisher. The publisher itself
 * is owned by FEstuaryLiveLinkModule via TSharedPtr<FEstuaryLiveLinkPublisher>; the bridge
 * does NOT manage the publisher's lifetime. UnregisterPublisher MUST be called from the
 * EstuaryLiveLink module's ShutdownModule (or world-teardown path) BEFORE the publisher
 * shared-ref is dropped.
 *
 * IsAvailable() is a pure runtime check — no WITH_ESTUARY_LIVELINK preprocessor gate
 * (the EstuaryLiveLink module is non-optional; LiveLink is engine-bundled on every
 * whitelisted platform).
 *
 * ZERO LiveLink/LiveLinkInterface includes — pure interface bridge (Pitfall 7 mirror of
 * FEstuaryLiveKitBridge shape; plain C++ interface kind, not UINTERFACE — see rationale
 * in IEstuaryLiveLinkPublisher above).
 */
class ESTUARY_API FEstuaryLiveLinkBridge
{
public:
    FEstuaryLiveLinkBridge()  = default;
    ~FEstuaryLiveLinkBridge() = default;

    // Non-copyable, non-movable (owned by UEstuarySubsystem as a value member).
    FEstuaryLiveLinkBridge(const FEstuaryLiveLinkBridge&)            = delete;
    FEstuaryLiveLinkBridge& operator=(const FEstuaryLiveLinkBridge&) = delete;
    FEstuaryLiveLinkBridge(FEstuaryLiveLinkBridge&&)                 = delete;
    FEstuaryLiveLinkBridge& operator=(FEstuaryLiveLinkBridge&&)      = delete;

    /**
     * Register the concrete publisher implementation. Called from
     * FEstuaryLiveLinkModule::OnPostWorldInit (mirroring EstuaryLiveKit lifecycle).
     *
     * The impl must remain alive for the plugin's lifetime. The EstuaryLiveLink
     * module owns it as a TSharedPtr<FEstuaryLiveLinkPublisher>. The bridge stores
     * a raw pointer only.
     * Must be called on the game thread.
     *
     * @param Impl  Non-null pointer to a concrete IEstuaryLiveLinkPublisher implementation.
     */
    void RegisterPublisher(IEstuaryLiveLinkPublisher* Impl)
    {
        check(Impl != nullptr);
        PublisherImpl = Impl;
    }

    /**
     * Unregister the publisher implementation. Called from
     * FEstuaryLiveLinkModule::ShutdownModule (or world-teardown path).
     * After this call, IsAvailable() returns false and GetPublisher() returns nullptr.
     * Must be called on the game thread. Safe to call when no impl is registered.
     */
    void UnregisterPublisher() { PublisherImpl = nullptr; }

    /**
     * Returns true when a publisher implementation has been registered at runtime.
     * Pure runtime check — no compile-time gate.
     * Must be called on the game thread.
     */
    bool IsAvailable() const                       { return PublisherImpl != nullptr; }

    /**
     * Get the registered publisher implementation.
     * Returns nullptr when not available (IsAvailable() == false).
     * Callers MUST check IsAvailable() or null-check the return before invoking methods.
     * Must be called on the game thread.
     */
    IEstuaryLiveLinkPublisher* GetPublisher() const { return PublisherImpl; }

    // ---------------------------------------------------------------------
    // Body publisher accessors (service-locator extension; parallel to face).
    // The body publisher is registered alongside the face publisher in the same
    // FEstuaryLiveLinkModule lifecycle (Pitfall 24 mirror — same OnPostWorldInit
    // + OnModularFeatureRegistered fallback). Both can be active simultaneously
    // per character (face on bare-id subject, body on "<characterId>_Body" subject).
    // ---------------------------------------------------------------------

    /**
     * Register the concrete body publisher implementation. Called from
     * FEstuaryLiveLinkModule::OnPostWorldInit alongside RegisterPublisher.
     *
     * The impl must remain alive for the plugin's lifetime. The EstuaryLiveLink
     * module owns it as a TSharedPtr<FEstuaryBodyLiveLinkPublisher>. The bridge
     * stores a raw pointer only.
     * Must be called on the game thread.
     *
     * @param Impl  Non-null pointer to a concrete IEstuaryBodyLiveLinkPublisher implementation.
     */
    void RegisterBodyPublisher(IEstuaryBodyLiveLinkPublisher* Impl)
    {
        check(Impl != nullptr);
        BodyPublisherImpl = Impl;
    }

    /**
     * Unregister the body publisher implementation. Called from
     * FEstuaryLiveLinkModule::ShutdownModule (or world-teardown path).
     * After this call, IsBodyAvailable() returns false and GetBodyPublisher() returns nullptr.
     * Must be called on the game thread. Safe to call when no impl is registered.
     */
    void UnregisterBodyPublisher() { BodyPublisherImpl = nullptr; }

    /**
     * Returns true when a body publisher implementation has been registered at runtime.
     * Pure runtime check — no compile-time gate. Independent of face IsAvailable().
     * Must be called on the game thread.
     */
    bool IsBodyAvailable() const { return BodyPublisherImpl != nullptr; }

    /**
     * Get the registered body publisher implementation.
     * Returns nullptr when not available (IsBodyAvailable() == false).
     * Callers MUST check IsBodyAvailable() or null-check the return before invoking methods.
     * Must be called on the game thread.
     */
    IEstuaryBodyLiveLinkPublisher* GetBodyPublisher() const { return BodyPublisherImpl; }

private:
    /**
     * Raw pointer to the registered face publisher impl.
     * Lifetime is managed by EstuaryLiveLink module (TSharedPtr<FEstuaryLiveLinkPublisher>).
     * We do NOT own this pointer. Set to nullptr in UnregisterPublisher.
     */
    IEstuaryLiveLinkPublisher* PublisherImpl = nullptr;

    /**
     * Raw pointer to the registered body publisher impl (service-locator extension).
     * Lifetime is managed by EstuaryLiveLink module (TSharedPtr<FEstuaryBodyLiveLinkPublisher>).
     * We do NOT own this pointer. Set to nullptr in UnregisterBodyPublisher.
     */
    IEstuaryBodyLiveLinkPublisher* BodyPublisherImpl = nullptr;
};
