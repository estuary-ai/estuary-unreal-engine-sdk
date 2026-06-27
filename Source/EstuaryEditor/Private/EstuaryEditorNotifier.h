// EstuaryEditorNotifier.h — Editor dependency notifier helper (Private — not in Public/ API).
// Shows a non-blocking Slate toast when the LiveKit WebRTC prebuilts are absent (API-06).

#pragma once

/**
 * FEstuaryEditorNotifier — Thin static helper that shows a Slate notification toast
 * at editor startup when the LiveKit WebRTC prebuilts are not installed.
 *
 * Call ShowIfNeeded() once from FEstuaryEditorModule::StartupModule via
 * FCoreDelegates::OnPostEngineInit so the Slate notification manager is ready.
 *
 * The notification is guarded by #if !WITH_ESTUARY_LIVEKIT — it fires only when
 * the ThirdParty/livekit-client-sdk-cpp directory (and hence the prebuilts) are absent.
 * When the prebuilts are present, WITH_ESTUARY_LIVEKIT is defined by EstuaryLiveKit.Build.cs
 * and this function is a no-op at compile time.
 */
struct FEstuaryEditorNotifier
{
    /**
     * Show the LiveKit-missing notification toast if WITH_ESTUARY_LIVEKIT is not defined.
     * Non-blocking (bFireAndForget=true, 12-second expire). No-op when LiveKit is present.
     */
    static void ShowIfNeeded();
};
