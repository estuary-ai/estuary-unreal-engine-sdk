// EstuaryConnectAction.h — Latent Blueprint node wrapping UEstuarySubsystem::Connect.
//
// Provides a "Connect (Latent)" node in the Blueprint palette under Estuary|Transport.
// Fires exactly one of four exec output pins (OnConnected, OnAuthFailed, OnRejected,
// OnNetworkError) per activation, then unbinds and self-destructs via SetReadyToDestroy.
//
// Pitfall 2:  All DYNAMIC_MULTICAST_DELEGATE receivers MUST be UFUNCTION() — enforced here.
// Pitfall 16: One-param USTRUCT payloads keep us well under the 8-param delegate cap.
// Pitfall 23: #pragma once, no CoreMinimal.h, no Engine.h.

#pragma once

#include "CoreTypes.h"
#include "UObject/ObjectMacros.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Containers/UnrealString.h"
#include "UObject/WeakObjectPtr.h"
#include "EstuaryTypes.h"

#include "EstuaryConnectAction.generated.h"

// Forward-declares to avoid pulling in subsystem header from this public header.
class UEstuarySubsystem;

// ---------------------------------------------------------------------------
// Output-pin delegate types — one per exec output.
// Each carries the minimal payload needed by the connected Blueprint node.
// ---------------------------------------------------------------------------

/** OnConnected output pin — fires when session_info is received. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEstuaryConnectActionConnectedPin,
    const FEstuarySessionInfo&, SessionInfo);

/** OnAuthFailed output pin — fires when auth_error is received. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEstuaryConnectActionAuthFailedPin,
    const FEstuaryAuthError&, AuthError);

/** OnRejected output pin — fires when session_rejected is received. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEstuaryConnectActionRejectedPin,
    const FEstuarySessionRejected&, Rejected);

/** OnNetworkError output pin — fires on transport-level failure with no session semantic event. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEstuaryConnectActionNetworkErrorPin,
    const FString&, Message);

// ---------------------------------------------------------------------------
// UEstuaryConnectAction
// ---------------------------------------------------------------------------

/**
 * Latent Blueprint node that wraps a single UEstuarySubsystem::Connect call.
 *
 * Blueprint usage:
 *   [Connect (Latent)] node appears in palette under Estuary|Transport.
 *   Wire the four output pins: OnConnected, OnAuthFailed, OnRejected, OnNetworkError.
 *   Exactly ONE pin fires per activation. After firing the action unbinds and is GC'd.
 *
 * C++ usage:
 *   Not intended for direct C++ use — consume UEstuarySubsystem directly instead.
 *
 * Guarantees:
 *   - Exactly one pin fires per activation (bCompleted single-fire guard).
 *   - All four subsystem delegates are unbound in Finish() before SetReadyToDestroy.
 *   - No dangling bindings across multiple activations.
 *   - Handlers are UFUNCTIONs (Pitfall 2: required for AddDynamic on DYNAMIC delegates).
 */
UCLASS()
class ESTUARY_API UEstuaryConnectAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:

    /**
     * Latent Connect — opens an Estuary session and fires exactly one output pin.
     *
     * @param WorldContextObject  Any UObject from the calling Blueprint (auto-wired).
     * @param ServerUrl           Gateway URL (Socket.IO over WebSocket), e.g. https://api.estuary-ai.com
     * @param Auth                Auth payload: ApiKey, CharacterId, PlayerId, AudioSampleRate.
     *
     * @return The async action object. Engine calls Activate() automatically.
     *         Returns nullptr if WorldContextObject is null (Blueprint will log a warning).
     */
    UFUNCTION(BlueprintCallable, Category = "Estuary|Transport",
              meta = (BlueprintInternalUseOnly = "true",
                      DisplayName = "Connect (Latent)",
                      ToolTip = "Open an Estuary session. Fires exactly one output pin: OnConnected on success, OnAuthFailed / OnRejected / OnNetworkError on failure.",
                      WorldContext = "WorldContextObject"))
    static UEstuaryConnectAction* Connect(UObject* WorldContextObject,
                                          const FString& ServerUrl,
                                          const FEstuaryAuthPayload& Auth);

    /** UBlueprintAsyncActionBase override — subscribes to subsystem delegates and calls Connect. */
    virtual void Activate() override;

    // -----------------------------------------------------------------------
    // Output pins (BlueprintAssignable properties render as exec output pins).
    // -----------------------------------------------------------------------

    /** Fires on session_info — session is live. Wire your next action here. */
    UPROPERTY(BlueprintAssignable,
              meta = (ToolTip = "Fires when session_info is received. Session is live."))
    FEstuaryConnectActionConnectedPin OnConnected;

    /** Fires on auth_error — API key / character_id / player_id rejected by the gateway. */
    UPROPERTY(BlueprintAssignable,
              meta = (ToolTip = "Fires when the gateway rejects the auth payload."))
    FEstuaryConnectActionAuthFailedPin OnAuthFailed;

    /** Fires on session_rejected — server policy cap (e.g., concurrent_limit). */
    UPROPERTY(BlueprintAssignable,
              meta = (ToolTip = "Fires when the gateway rejects the session due to a server-side policy cap."))
    FEstuaryConnectActionRejectedPin OnRejected;

    /** Fires on transport-level failure with no auth/policy semantic from the server.
     *  Message contains a short diagnostic string (e.g., \"transport_error\"). */
    UPROPERTY(BlueprintAssignable,
              meta = (ToolTip = "Fires on a transport-level failure with no session semantic event from the server."))
    FEstuaryConnectActionNetworkErrorPin OnNetworkError;

private:

    // Cached from the factory; populated before Activate() is called.
    /** UPROPERTY so the GC can track the WorldContext reference lifecycle. */
    UPROPERTY()
    TWeakObjectPtr<UObject> WorldContext;

    FString             CachedServerUrl;
    FEstuaryAuthPayload CachedAuth;

    /** Weak reference to the subsystem. NOT UPROPERTY — we do not root the subsystem;
     *  its lifetime is tied to the GameInstance. We rely on explicit Unbind* in Finish(). */
    TWeakObjectPtr<UEstuarySubsystem> SubsystemWeak;

    /** Single-fire guard. Once true, all subsequent handler calls are no-ops. */
    bool bCompleted = false;

    // -----------------------------------------------------------------------
    // Handler UFUNCTIONs — Pitfall 2: UFUNCTION() is REQUIRED on every method
    // bound via AddDynamic to a DYNAMIC_MULTICAST_DELEGATE.
    // -----------------------------------------------------------------------

    /** Handles OnSessionConnected from UEstuarySubsystem. */
    UFUNCTION()
    void HandleSessionConnected(const FEstuarySessionInfo& Info);

    /** Handles OnAuthError from UEstuarySubsystem. */
    UFUNCTION()
    void HandleAuthError(const FEstuaryAuthError& Err);

    /** Handles OnSessionRejected from UEstuarySubsystem. */
    UFUNCTION()
    void HandleSessionRejected(const FEstuarySessionRejected& Rej);

    /** Handles OnStateChanged from UEstuarySubsystem.
     *  Routes EEstuarySessionState::Error (with no prior terminal event) to OnNetworkError. */
    UFUNCTION()
    void HandleStateChanged(EEstuarySessionState NewState);

    /** Unbinds all four subsystem delegates and calls SetReadyToDestroy(). */
    void Finish();
};
