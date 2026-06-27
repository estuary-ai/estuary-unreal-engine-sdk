// EstuaryConnectAction.cpp — Latent BP node wrapping UEstuarySubsystem::Connect.
//
// Lifecycle:
//   1. Connect() (static factory) — creates action, caches params.
//   2. Activate() — resolves subsystem, binds 4 delegates, calls Subsystem->Connect().
//   3. One of HandleSessionConnected / HandleAuthError / HandleSessionRejected /
//      HandleStateChanged fires (with bCompleted guard ensuring exactly one fires).
//   4. Finish() — unbinds all 4 delegates, calls SetReadyToDestroy().
//   5. GC collects the action on the next pass.
//
// Guarantees:
//   - Exactly one output pin fires per activation (bCompleted bool).
//   - All 4 delegates are removed in Finish() — no dangling bindings.
//   - HandleStateChanged: only OnNetworkError path fires; Error state after a prior
//     terminal event (OnAuthError / OnSessionRejected) is already bCompleted=true.
//
// Note on Reconnecting: this action does NOT complete on Reconnecting. The latent
// node represents a single user-initiated connect attempt. Reconnect cycles run
// transparently inside UEstuarySubsystem; the node waits for the final outcome.
// For persistent reconnect monitoring, bind UEstuarySubsystem::OnStateChanged directly.

#include "EstuaryConnectAction.h"
#include "EstuarySubsystem.h"
#include "EstuaryLog.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

// ============================================================================
// Static factory
// ============================================================================

UEstuaryConnectAction* UEstuaryConnectAction::Connect(UObject* WorldContextObject,
                                                      const FString& ServerUrl,
                                                      const FEstuaryAuthPayload& Auth)
{
    if (!WorldContextObject)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[ConnectAction] Connect factory: WorldContextObject is null — returning nullptr"));
        return nullptr;
    }

    UEstuaryConnectAction* Action = NewObject<UEstuaryConnectAction>();
    Action->WorldContext     = WorldContextObject;
    Action->CachedServerUrl  = ServerUrl;
    Action->CachedAuth       = Auth;
    return Action;
}

// ============================================================================
// Activate
// ============================================================================

void UEstuaryConnectAction::Activate()
{
    UE_LOG(LogEstuary, Log,
        TEXT("[ConnectAction] Activate ENTER — ServerUrl=%s charId=%s"),
        *CachedServerUrl, *CachedAuth.CharacterId);

    check(IsInGameThread());
    bCompleted = false;

    // Resolve WorldContext.
    UObject* Ctx = WorldContext.Get();
    if (!Ctx)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[ConnectAction] Activate: WorldContext GC'd before Activate() — firing OnNetworkError"));
        OnNetworkError.Broadcast(TEXT("invalid_world_context"));
        SetReadyToDestroy();
        return;
    }

    // Resolve GameInstance.
    UGameInstance* GI = nullptr;
    if (GEngine)
    {
        UWorld* World = GEngine->GetWorldFromContextObject(Ctx, EGetWorldErrorMode::LogAndReturnNull);
        if (World)
        {
            GI = World->GetGameInstance();
        }
    }

    if (!GI)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[ConnectAction] Activate: no GameInstance available — firing OnNetworkError"));
        OnNetworkError.Broadcast(TEXT("no_game_instance"));
        SetReadyToDestroy();
        return;
    }

    // Resolve subsystem.
    UEstuarySubsystem* Subsystem = GI->GetSubsystem<UEstuarySubsystem>();
    if (!Subsystem)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[ConnectAction] Activate: UEstuarySubsystem not found — firing OnNetworkError"));
        OnNetworkError.Broadcast(TEXT("subsystem_missing"));
        SetReadyToDestroy();
        return;
    }
    SubsystemWeak = Subsystem;

    UE_LOG(LogEstuary, Log,
        TEXT("[ConnectAction] Activate: subsystem resolved — binding delegates and calling Connect"));

    // Bind delegates BEFORE calling Connect so we cannot miss the first transition.
    Subsystem->OnSessionConnected.AddDynamic(this, &UEstuaryConnectAction::HandleSessionConnected);
    Subsystem->OnAuthError.AddDynamic(this,        &UEstuaryConnectAction::HandleAuthError);
    Subsystem->OnSessionRejected.AddDynamic(this,  &UEstuaryConnectAction::HandleSessionRejected);
    Subsystem->OnStateChanged.AddDynamic(this,     &UEstuaryConnectAction::HandleStateChanged);

    // Kick off the connection.
    Subsystem->Connect(CachedServerUrl, CachedAuth);

    UE_LOG(LogEstuary, Log,
        TEXT("[ConnectAction] Activate: Subsystem->Connect returned"));
}

// ============================================================================
// Handlers — single-fire guard (bCompleted) on every path
// ============================================================================

void UEstuaryConnectAction::HandleSessionConnected(const FEstuarySessionInfo& Info)
{
    if (bCompleted) { return; }
    bCompleted = true;

    UE_LOG(LogEstuary, Log,
        TEXT("[ConnectAction] OnConnected — session=%s conversation=%s"),
        *Info.SessionId, *Info.ConversationId);

    OnConnected.Broadcast(Info);
    Finish();
}

void UEstuaryConnectAction::HandleAuthError(const FEstuaryAuthError& Err)
{
    if (bCompleted) { return; }
    bCompleted = true;

    UE_LOG(LogEstuary, Log,
        TEXT("[ConnectAction] OnAuthFailed — %s"), *Err.Error);

    OnAuthFailed.Broadcast(Err);
    Finish();
}

void UEstuaryConnectAction::HandleSessionRejected(const FEstuarySessionRejected& Rej)
{
    if (bCompleted) { return; }
    bCompleted = true;

    UE_LOG(LogEstuary, Log,
        TEXT("[ConnectAction] OnRejected — reason=%s cap=%d"), *Rej.Reason, Rej.Cap);

    OnRejected.Broadcast(Rej);
    Finish();
}

void UEstuaryConnectAction::HandleStateChanged(EEstuarySessionState NewState)
{
    // The three semantic outcomes (OnConnected / OnAuthFailed / OnRejected) have their
    // own dedicated delegates and will have set bCompleted=true before this handler
    // would observe the Error state they caused. This handler only fires OnNetworkError
    // for a pure transport failure — i.e., Error state with no prior server semantic event.
    //
    // Reconnecting is NOT a terminal outcome for this node. The action keeps listening
    // through reconnect cycles until one of the four terminal outcomes fires.
    if (bCompleted) { return; }

    if (NewState == EEstuarySessionState::Error)
    {
        bCompleted = true;

        UE_LOG(LogEstuary, Log,
            TEXT("[ConnectAction] OnNetworkError — subsystem transitioned to Error with no prior auth/rejection event"));

        OnNetworkError.Broadcast(TEXT("transport_error"));
        Finish();
    }
    // Reconnecting / Connecting / Connected state changes during an in-progress
    // attempt are silently ignored — only terminal states complete this action.
}

// ============================================================================
// Finish — unbind all four delegates, then signal GC
// ============================================================================

void UEstuaryConnectAction::Finish()
{
    check(IsInGameThread());

    if (UEstuarySubsystem* Subsystem = SubsystemWeak.Get())
    {
        Subsystem->OnSessionConnected.RemoveDynamic(this, &UEstuaryConnectAction::HandleSessionConnected);
        Subsystem->OnAuthError.RemoveDynamic(this,        &UEstuaryConnectAction::HandleAuthError);
        Subsystem->OnSessionRejected.RemoveDynamic(this,  &UEstuaryConnectAction::HandleSessionRejected);
        Subsystem->OnStateChanged.RemoveDynamic(this,     &UEstuaryConnectAction::HandleStateChanged);
    }
    // If the subsystem is already gone (e.g., PIE stop while action was live),
    // the delegates were cleared by the subsystem's own Deinitialize. No leak.

    SetReadyToDestroy();  // signals the engine to GC this action on the next pass
}
