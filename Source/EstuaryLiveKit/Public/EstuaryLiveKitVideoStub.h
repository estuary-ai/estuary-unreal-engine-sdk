// EstuaryLiveKitVideoStub.h — v2 placeholder implementing IEstuaryLiveKitVideo.
// No video functionality in v1. Declared so the interface is not abstract-only,
// and so Plan 08-01's RegisterVideoImpl has a concrete type available if needed.
//
// Pitfall 23: #pragma once, IWYU, no CoreMinimal.h.
#pragma once

#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "EstuaryLiveKitBridge.h"

#include "EstuaryLiveKitVideoStub.generated.h"

/**
 * v2 placeholder for LiveKit video track support.
 *
 * Not registered with FEstuaryLiveKitBridge in v1 — video is deferred to v2.
 * Declared here to satisfy the IEstuaryLiveKitVideo pure virtual interface
 * and to give type-safe instantiation in v2 without changing the bridge API.
 *
 * v2 implementation checklist (do NOT implement until v2 scope is defined):
 *   1. Replace StartVideo() with livekit::Room::PublishVideoTrack() call
 *   2. Replace StopVideo() with livekit::LocalVideoTrack::Stop() call
 *   3. Register with FEstuaryLiveKitBridge::RegisterVideoImpl in StartupModule
 *   4. Unregister in ShutdownModule (same lifecycle as VoiceImpl)
 *   5. Document the IEstuaryLiveKitVideo platform capability in the SDK docs
 */
UCLASS()
class ESTUARYLIVEKIT_API UEstuaryLiveKitVideoStub : public UObject, public IEstuaryLiveKitVideo
{
    GENERATED_BODY()

public:
    /**
     * v2: Start publishing a video track. Not implemented in v1.
     * Remove this stub and replace with real implementation in v2.
     */
    virtual void StartVideo() override
    {
        // v2 TODO: create and publish a local video track via livekit::Room.
        // Expected API (based on LK JS SDK parity):
        //   livekit::LocalVideoTrack* Track = livekit::LocalVideoTrack::CreateCameraTrack();
        //   Room->GetLocalParticipant()->PublishVideoTrack(Track);
    }

    /**
     * v2: Stop publishing the video track. Not implemented in v1.
     */
    virtual void StopVideo() override
    {
        // v2 TODO: unpublish and stop the local video track:
        //   Room->GetLocalParticipant()->UnpublishVideoTrack(Track);
        //   Track->Stop();
    }
};
