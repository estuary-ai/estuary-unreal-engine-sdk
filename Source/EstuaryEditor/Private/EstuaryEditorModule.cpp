// EstuaryEditorModule.cpp — Editor module entry point.
// Path B: auto-installer removed; Estuary core no longer depends on sibling plugins.
// Notifier surfaces only the optional LiveKit prebuilts state.
#include "EstuaryEditorModule.h"
#include "EstuaryEditorNotifier.h"
#include "EstuaryLog.h"
#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"

// Store the delegate handle so we can remove it in ShutdownModule.
// Static because only one editor module instance exists; no UObject heap involved.
static FDelegateHandle GPostEngineInitHandle;

void FEstuaryEditorModule::StartupModule()
{
    UE_LOG(LogEstuary, Log, TEXT("EstuaryEditor module started"));

    // FCoreDelegates::OnPostEngineInit fires after the editor's engine subsystems are
    // ready — safe to call Slate notification APIs here. Using a one-shot lambda:
    // after the notification fires, we remove the delegate handle so no re-fire occurs
    // on hot-reload. ShutdownModule also removes it defensively.
    GPostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda([]()
    {
        // Surfaces optional-LiveKit-missing toast (no auto-installer; sibling plugins
        // are gone in Path B — only the LiveKit ThirdParty prebuilts can be missing).
        FEstuaryEditorNotifier::ShowIfNeeded();

        // One-shot: remove immediately after firing so we don't fire again on hot-reload.
        FCoreDelegates::OnPostEngineInit.Remove(GPostEngineInitHandle);
        GPostEngineInitHandle.Reset();
    });
}

void FEstuaryEditorModule::ShutdownModule()
{
    UE_LOG(LogEstuary, Log, TEXT("EstuaryEditor module shut down"));

    // T-09-02-01: Defensive cleanup in case the module is shut down before the
    // OnPostEngineInit delegate had a chance to fire (e.g. early engine teardown).
    if (GPostEngineInitHandle.IsValid())
    {
        FCoreDelegates::OnPostEngineInit.Remove(GPostEngineInitHandle);
        GPostEngineInitHandle.Reset();
    }
}

IMPLEMENT_MODULE(FEstuaryEditorModule, EstuaryEditor)
