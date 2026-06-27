// EstuaryEditorNotifier.cpp — Slate notification toast for missing optional dependencies.
// Non-blocking: bFireAndForget=true prevents input-focus steal.
// FMessageDialog::Open is explicitly NOT used here — that would be a blocking modal.
//
// Path B: only one toast remains — !WITH_ESTUARY_LIVEKIT (optional WebRTC voice).
// Transport (Socket.IO over UE built-in WebSockets) and mic (UAudioCaptureComponent)
// are now in-plugin and always available; no sibling-plugin probes.

#include "EstuaryEditorNotifier.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Internationalization/Text.h"

void FEstuaryEditorNotifier::ShowIfNeeded()
{
#if !WITH_ESTUARY_LIVEKIT
    FNotificationInfo Info(FText::FromString(TEXT(
        "Estuary: LiveKit WebRTC voice is unavailable. "
        "To enable desktop WebRTC, drop livekit/client-sdk-cpp prebuilts into "
        "Plugins/Estuary/ThirdParty/livekit-client-sdk-cpp/. "
        "WebSocket voice is active as fallback."
    )));
    Info.bFireAndForget  = true;
    Info.FadeInDuration  = 0.5f;
    Info.FadeOutDuration = 2.0f;
    Info.ExpireDuration  = 12.0f;
    Info.bUseLargeFont   = false;
    FSlateNotificationManager::Get().AddNotification(Info);
#endif
}
