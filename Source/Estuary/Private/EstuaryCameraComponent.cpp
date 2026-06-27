// EstuaryCameraComponent.cpp — Scene capture + JPEG encode + camera_image emit.
//
// Pitfall 23: IWYU — no CoreMinimal.h, no Engine.h.
// Pitfall 1:  All UObject touches on the game thread.
// Pitfall 3:  TWeakObjectPtr captures in all lambdas that cross thread boundaries.
// Pitfall 7:  Zero LiveKit references.
// Pitfall 17: check(IsInGameThread()) at entry of every game-thread method.
// T-07-02:   ImageJpegBase64 never logged at any level.
// T-07-04:   No capture or emit path bypasses the bHasUserConsent check.
// T-07-05:   TAtomic<bool> bIsEncoding prevents concurrent encode stacking.
// T-07-06:   HandleCameraCaptureFromSubsystem checks consent before DoCapture.

#include "EstuaryCameraComponent.h"
#include "EstuarySubsystem.h"
#include "EstuaryCharacter.h"
#include "EstuaryLog.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "Engine/GameInstance.h"
#include "Async/Async.h"
#include "Misc/Base64.h"
#include "Misc/Guid.h"
#include "HAL/PlatformTime.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"

// Webcam (device-camera) mode — UE Media Framework + blit material.
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "MediaCaptureSupport.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "UObject/SoftObjectPtr.h"

// ============================================================================
// Constructor
// ============================================================================

UEstuaryCameraComponent::UEstuaryCameraComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    bWantsInitializeComponent = true;

    // Create SceneCapture as a default subobject and attach it here in the
    // constructor. SetupAttachment is ONLY valid pre-registration — it records
    // AttachParent for the engine to wire up during registration. Calling it
    // from InitializeComponent (post-registration) trips ensure(!bRegistered)
    // in SceneComponent.cpp; post-registration you'd have to use AttachToComponent.
    SceneCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCapture"));
    SceneCapture->SetupAttachment(this);

    // Manual-only capture — we call CaptureScene() explicitly in DoCapture.
    SceneCapture->bCaptureEveryFrame = false;
    SceneCapture->bCaptureOnMovement = false;
}

// ============================================================================
// InitializeComponent — create render target, attach SceneCapture
// ============================================================================

void UEstuaryCameraComponent::InitializeComponent()
{
    Super::InitializeComponent();

    // Create the render target. 512x512 RGBA8 default. Resize via SetRenderTargetSize().
    RenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("EstuaryCameraRT"));
    RenderTarget->RenderTargetFormat = RTF_RGBA8;
    RenderTarget->InitAutoFormat(512, 512);
    RenderTarget->UpdateResourceImmediate(true);

    // Assign RT to the scene capture. Attachment was set up in the constructor
    // (SetupAttachment is illegal here — the subobject is already registered).
    if (SceneCapture)
    {
        SceneCapture->TextureTarget = RenderTarget;
    }

    UE_LOG(LogEstuary, Verbose,
        TEXT("[CameraComponent] InitializeComponent: RT 512x512 created, SceneCapture target assigned."));
}

// ============================================================================
// BeginPlay — register with subsystem
// ============================================================================

void UEstuaryCameraComponent::BeginPlay()
{
    Super::BeginPlay();
    check(IsInGameThread());

    UGameInstance* GI = GetOwner() ? GetOwner()->GetGameInstance() : nullptr;
    if (!IsValid(GI))
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[CameraComponent] BeginPlay: no GameInstance — cannot register."));
        return;
    }

    UEstuarySubsystem* Sub = GI->GetSubsystem<UEstuarySubsystem>();
    if (!IsValid(Sub))
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[CameraComponent] BeginPlay: UEstuarySubsystem not found — cannot register."));
        return;
    }

    Sub->RegisterCameraComponent(this);
    bIsRegistered = true;

    UE_LOG(LogEstuary, Log,
        TEXT("[CameraComponent] Registered with subsystem (char='%s')."), *GetCharacterId());
}

// ============================================================================
// EndPlay — unregister from subsystem
// ============================================================================

void UEstuaryCameraComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    check(IsInGameThread());

    // Release any open webcam device before teardown.
    StopWebcam();

    if (bIsRegistered)
    {
        UGameInstance* GI = GetOwner() ? GetOwner()->GetGameInstance() : nullptr;
        if (IsValid(GI))
        {
            UEstuarySubsystem* Sub = GI->GetSubsystem<UEstuarySubsystem>();
            if (IsValid(Sub))
            {
                Sub->UnregisterCameraComponent(this);
            }
        }
        bIsRegistered = false;
        UE_LOG(LogEstuary, Log,
            TEXT("[CameraComponent] Unregistered from subsystem (char='%s')."), *GetCharacterId());
    }

    Super::EndPlay(EndPlayReason);
}

// ============================================================================
// SetCameraConsent (CAM-04)
// ============================================================================

void UEstuaryCameraComponent::SetCameraConsent(bool bGranted)
{
    check(IsInGameThread());
    bHasUserConsent = bGranted;
    UE_LOG(LogEstuary, Log,
        TEXT("[CameraComponent] Consent %s (char='%s')."),
        bGranted ? TEXT("GRANTED") : TEXT("REVOKED"),
        *GetCharacterId());
}

// ============================================================================
// SendCameraFrame (CAM-03, CAM-04) — game-thread entry point
// ============================================================================

bool UEstuaryCameraComponent::SendCameraFrame()
{
    check(IsInGameThread()); // Pitfall 1

    // T-07-04: gate on consent — no capture without explicit user grant.
    if (!bHasUserConsent)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[CameraComponent] SendCameraFrame called without user consent — no frame sent. "
                 "Call SetCameraConsent(true) first (char='%s')."),
            *GetCharacterId());
        return false;
    }

    // T-07-05: atomic guard — drop concurrent request if encode already in flight.
    if (bIsEncoding.Load())
    {
        UE_LOG(LogEstuary, Verbose,
            TEXT("[CameraComponent] SendCameraFrame: encode in flight — request dropped (char='%s')."),
            *GetCharacterId());
        return false;
    }

    // Generate a fresh client-side request_id (EGuidFormats::Digits = 32-char hex, no dashes).
    DoCapture(FGuid::NewGuid().ToString(EGuidFormats::Digits), TEXT(""));
    return true;
}

// ============================================================================
// SetRenderTargetSize
// ============================================================================

bool UEstuaryCameraComponent::SetRenderTargetSize(int32 Width, int32 Height)
{
    check(IsInGameThread());

    if (Width <= 0 || Height <= 0)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[CameraComponent] SetRenderTargetSize: invalid dimensions %dx%d — ignored."),
            Width, Height);
        return false;
    }

    if (!RenderTarget)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[CameraComponent] SetRenderTargetSize: RenderTarget is null — call InitializeComponent first."));
        return false;
    }

    RenderTarget->ResizeTarget(Width, Height);
    UE_LOG(LogEstuary, Verbose,
        TEXT("[CameraComponent] RenderTarget resized to %dx%d."), Width, Height);
    return true;
}

// ============================================================================
// SetCaptureSource (CAM-07) — choose which camera feeds the VLM
// ============================================================================

void UEstuaryCameraComponent::SetCaptureSource(EEstuaryCaptureSource NewSource)
{
    check(IsInGameThread());

    CaptureSource = NewSource;

    // Warm up the webcam the moment the mode selects it, so a later capture
    // (manual or server-initiated) has a live feed ready. StartWebcam early-outs
    // if the device is already open.
    if (NewSource == EEstuaryCaptureSource::Webcam && !bWebcamOpen)
    {
        StartWebcam(0);
    }

    UE_LOG(LogEstuary, Log,
        TEXT("[CameraComponent] CaptureSource = %s (char='%s')."),
        NewSource == EEstuaryCaptureSource::Webcam ? TEXT("Webcam") : TEXT("Rendered Scene"),
        *GetCharacterId());
}

// ============================================================================
// HandleCameraCaptureFromSubsystem (CAM-03 — server-initiated path)
// ============================================================================

void UEstuaryCameraComponent::HandleCameraCaptureFromSubsystem(const FEstuaryCameraCapture& Capture)
{
    check(IsInGameThread());

    // T-07-06: consent check — server cannot force a capture without prior consent.
    if (!bHasUserConsent)
    {
        UE_LOG(LogEstuary, Verbose,
            TEXT("[CameraComponent] camera_capture received but consent not granted — ignoring "
                 "(char='%s')."), *GetCharacterId());
        return;
    }

    // T-07-05: atomic guard — drop if encode already in flight.
    if (bIsEncoding.Load())
    {
        UE_LOG(LogEstuary, Verbose,
            TEXT("[CameraComponent] camera_capture: encode in flight — request dropped (char='%s')."),
            *GetCharacterId());
        return;
    }

    // CAM-07: in Webcam mode, capture from the device. If the feed isn't ready
    // (device still warming up), fall back to the rendered scene so the
    // conversation still gets a frame to reason about.
    if (CaptureSource == EEstuaryCaptureSource::Webcam)
    {
        if (DoWebcamCapture(Capture.RequestId, Capture.Text))
        {
            return;
        }
        UE_LOG(LogEstuary, Warning,
            TEXT("[CameraComponent] camera_capture: Webcam mode but feed unavailable — "
                 "falling back to rendered scene (char='%s')."), *GetCharacterId());
    }

    DoCapture(Capture.RequestId, Capture.Text);
}

// ============================================================================
// DoCapture — internal capture + encode + emit pipeline
//
// Must be called on the game thread (consent + bIsEncoding guards are the caller's
// responsibility — both SendCameraFrame and HandleCameraCaptureFromSubsystem enforce this).
// ============================================================================

void UEstuaryCameraComponent::DoCapture(const FString& RequestId, const FString& ContextText)
{
    check(IsInGameThread()); // Belt-and-suspenders — every caller checks too.

    if (!SceneCapture || !RenderTarget)
    {
        UE_LOG(LogEstuary, Error,
            TEXT("[CameraComponent] DoCapture: SceneCapture or RenderTarget is null — cannot capture."));
        return;
    }

    // --- Step 1: synchronous scene render into the RT (game thread). ---
    // CaptureScene() is the correct UE 5.5 API for manual-trigger captures.
    SceneCapture->CaptureScene();

    // --- Step 2: flush rendering commands so RT is fully populated before readback. ---
    FlushRenderingCommands();

    // --- Step 3: read pixels from RT into a TArray<FColor> (game thread). ---
    // UTextureRenderTarget2D doesn't expose ReadPixels directly; the read goes
    // through the FTextureRenderTargetResource (the GPU-side resource).
    TArray<FColor> Pixels;
    FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
    const bool bReadOk = RTResource && RTResource->ReadPixels(Pixels);
    if (!bReadOk || Pixels.Num() == 0)
    {
        UE_LOG(LogEstuary, Error,
            TEXT("[CameraComponent] DoCapture: ReadPixels failed or returned empty array — aborting."));
        return;
    }

    const int32 W = RenderTarget->SizeX;
    const int32 H = RenderTarget->SizeY;

    // --- Step 4: hand the captured pixels to the shared encode + emit tail. ---
    EncodeAndEmitPixels(MoveTemp(Pixels), W, H, RequestId, ContextText);
}

// ============================================================================
// EncodeAndEmitPixels — shared encode + emit tail (scene capture + webcam)
//
// Marks the encode in flight, JPEG-encodes the BGRA pixels off the game thread,
// then marshals back to emit camera_image. The caller must already hold the
// consent + bIsEncoding guards (SendCameraFrame / HandleCameraCaptureFromSubsystem
// / SendWebcamFrame all do).
// ============================================================================

void UEstuaryCameraComponent::EncodeAndEmitPixels(TArray<FColor>&& Pixels, int32 Width, int32 Height,
                                                  const FString& RequestId, const FString& ContextText)
{
    check(IsInGameThread());

    // Mark encode in flight; cleared on the game thread once emit completes or on error.
    bIsEncoding.Store(true);

    TWeakObjectPtr<UEstuaryCameraComponent> WeakThis(this);
    const int32   W           = Width;
    const int32   H           = Height;
    const int32   LocalQuality = JpegQuality;
    const FString LocalId      = RequestId;
    const FString LocalText    = ContextText;
    const FString LocalCharId  = GetCharacterId();

    AsyncTask(ENamedThreads::AnyThread,
        [WeakThis, Pixels = MoveTemp(Pixels), W, H, LocalQuality, LocalId, LocalText, LocalCharId]()
    {
        // --- Step 5 (AnyThread): JPEG encode via IImageWrapperModule. ---
        // CAM-02: the expensive GetCompressed() call runs off the game thread.
        IImageWrapperModule& ImageWrapperModule =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

        TSharedPtr<IImageWrapper> Wrapper =
            ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);

        if (!Wrapper.IsValid())
        {
            UE_LOG(LogEstuary, Error, TEXT("[CameraComponent] DoCapture: failed to create JPEG wrapper."));
            // Clear the guard and abort — must marshal back to GT to touch bIsEncoding safely.
            AsyncTask(ENamedThreads::GameThread, [WeakThis]()
            {
                if (UEstuaryCameraComponent* Self = WeakThis.Get())
                {
                    Self->bIsEncoding.Store(false);
                }
            });
            return;
        }

        // FColor in UE is BGRA layout. Use ERGBFormat::BGRA accordingly.
        Wrapper->SetRaw(
            Pixels.GetData(),
            static_cast<int64>(Pixels.Num()) * static_cast<int64>(sizeof(FColor)),
            W, H,
            ERGBFormat::BGRA,
            8);

        // GetCompressed() returns const TArray64<uint8>& (UE 5.5 API).
        const TArray64<uint8>& CompressedData = Wrapper->GetCompressed(LocalQuality);

        if (CompressedData.Num() == 0)
        {
            UE_LOG(LogEstuary, Error, TEXT("[CameraComponent] DoCapture: JPEG compression produced empty output."));
            AsyncTask(ENamedThreads::GameThread, [WeakThis]()
            {
                if (UEstuaryCameraComponent* Self = WeakThis.Get())
                {
                    Self->bIsEncoding.Store(false);
                }
            });
            return;
        }

        // --- Step 6: Base64-encode the JPEG bytes. ---
        // Non-const because we MoveTemp it into the AsyncTask lambda below
        // (UE 5.7+ static_asserts MoveTemp on a const value).
        FString Base64Image = FBase64::Encode(
            reinterpret_cast<const uint8*>(CompressedData.GetData()),
            static_cast<uint32>(CompressedData.Num()));

        // --- Step 7: Marshal back to the game thread to emit. ---
        // Pitfall 1 + Pitfall 3: TWeakObjectPtr guard before touching any UObject.
        AsyncTask(ENamedThreads::GameThread,
            [WeakThis, Base64Image = MoveTemp(Base64Image), W, H, LocalId, LocalText, LocalCharId]()
        {
            check(IsInGameThread());

            // Pitfall 3: component may have been destroyed during the encode.
            UEstuaryCameraComponent* Self = WeakThis.Get();
            if (!Self)
            {
                // Component gone — nothing to do; bIsEncoding no longer matters.
                return;
            }

            Self->bIsEncoding.Store(false);

            // Build the wire payload.
            FEstuaryCameraImage Payload;
            Payload.ImageJpegBase64 = Base64Image;
            Payload.MimeType        = TEXT("image/jpeg");
            Payload.RequestId       = LocalId;
            Payload.Text            = LocalText;
            Payload.CharacterId     = LocalCharId;
            Payload.Width           = W;
            Payload.Height          = H;
            // Wall-clock timestamp in ms — informational only, not on the wire.
            Payload.TimestampMs = static_cast<int64>(
                FPlatformTime::Cycles64() *
                FPlatformTime::GetSecondsPerCycle64() * 1000.0);

            // Route through the subsystem which emits via the bridge.
            UGameInstance* GI = Self->GetOwner()
                ? Self->GetOwner()->GetGameInstance() : nullptr;
            if (!IsValid(GI)) { return; }

            UEstuarySubsystem* Sub = GI->GetSubsystem<UEstuarySubsystem>();
            if (!IsValid(Sub)) { return; }

            Sub->EmitCameraImage(Payload);

            // T-07-02: do NOT log Payload.ImageJpegBase64 at any level.
            UE_LOG(LogEstuary, Verbose,
                TEXT("[CameraComponent] DoCapture complete: request_id=%s %dx%d "
                     "b64_len=%d char='%s'"),
                *LocalId, W, H, Payload.ImageJpegBase64.Len(), *LocalCharId);
        });
    });
}

// ============================================================================
// StartWebcam (CAM-07) — open a physical video-capture device via Media Framework
// ============================================================================

bool UEstuaryCameraComponent::StartWebcam(int32 DeviceIndex)
{
    check(IsInGameThread());

    if (bWebcamOpen)
    {
        UE_LOG(LogEstuary, Verbose,
            TEXT("[CameraComponent] StartWebcam: already open (char='%s')."), *GetCharacterId());
        return true;
    }

    // Enumerate physical video-capture devices (MediaUtils / platform media players).
    TArray<FMediaCaptureDeviceInfo> Devices;
    MediaCaptureSupport::EnumerateVideoCaptureDevices(Devices);
    if (Devices.Num() == 0)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[CameraComponent] StartWebcam: no video-capture devices found."));
        return false;
    }
    if (DeviceIndex < 0 || DeviceIndex >= Devices.Num())
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[CameraComponent] StartWebcam: DeviceIndex %d out of range (%d devices) — using 0."),
            DeviceIndex, Devices.Num());
        DeviceIndex = 0;
    }
    const FMediaCaptureDeviceInfo& Device = Devices[DeviceIndex];

    // Create the media player + texture (transient, owned by this component). UMediaPlayer
    // auto-plays on open (PlayOnOpen defaults true), so the feed begins warming up immediately.
    WebcamPlayer = NewObject<UMediaPlayer>(this, NAME_None, RF_Transient);
    WebcamTexture = NewObject<UMediaTexture>(this, NAME_None, RF_Transient);
    WebcamTexture->SetMediaPlayer(WebcamPlayer);
    WebcamTexture->UpdateResource();

    // Resolve the blit material (explicit override first, else the default project material).
    UMaterialInterface* BlitMat = WebcamBlitMaterial.LoadSynchronous();
    if (!BlitMat)
    {
        BlitMat = LoadObject<UMaterialInterface>(
            nullptr, TEXT("/Game/Materials/M_EstuaryWebcamBlit.M_EstuaryWebcamBlit"));
    }
    if (!BlitMat)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[CameraComponent] StartWebcam: webcam blit material not found "
                 "(set WebcamBlitMaterial or create /Game/Materials/M_EstuaryWebcamBlit) — aborting."));
        StopWebcam();
        return false;
    }
    WebcamBlitMID = UMaterialInstanceDynamic::Create(BlitMat, this);
    WebcamBlitMID->SetTextureParameterValue(TEXT("MediaTex"), WebcamTexture);

    // Open the device. The decoded feed appears over the next frames (warm-up).
    if (!WebcamPlayer->OpenUrl(Device.Url))
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[CameraComponent] StartWebcam: OpenUrl failed for device '%s' (url=%s)."),
            *Device.DisplayName.ToString(), *Device.Url);
        StopWebcam();
        return false;
    }

    bWebcamOpen = true;
    UE_LOG(LogEstuary, Log,
        TEXT("[CameraComponent] StartWebcam: opened '%s' (char='%s'). Feed warming up."),
        *Device.DisplayName.ToString(), *GetCharacterId());
    return true;
}

// ============================================================================
// StopWebcam — close the device and release media objects
// ============================================================================

void UEstuaryCameraComponent::StopWebcam()
{
    check(IsInGameThread());

    if (WebcamPlayer)
    {
        WebcamPlayer->Close();
    }
    WebcamPlayer  = nullptr;
    WebcamTexture = nullptr;
    WebcamBlitMID = nullptr;
    // WebcamRT is retained for reuse across start/stop cycles; GC reclaims it with the component.
    bWebcamOpen = false;
}

// ============================================================================
// SendWebcamFrame (CAM-07) — grab the latest webcam frame and emit camera_image
//
// Webcam analogue of SendCameraFrame: instead of rendering the scene, it blits the
// latest decoded media frame into a render target and reuses EncodeAndEmitPixels.
// ============================================================================

bool UEstuaryCameraComponent::SendWebcamFrame()
{
    check(IsInGameThread()); // Pitfall 1

    // T-07-04: gate on consent — same rule as scene capture.
    if (!bHasUserConsent)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[CameraComponent] SendWebcamFrame called without user consent — no frame sent. "
                 "Call SetCameraConsent(true) first (char='%s')."),
            *GetCharacterId());
        return false;
    }

    // T-07-05: atomic guard — drop if an encode (scene or webcam) is already in flight.
    if (bIsEncoding.Load())
    {
        UE_LOG(LogEstuary, Verbose,
            TEXT("[CameraComponent] SendWebcamFrame: encode in flight — request dropped (char='%s')."),
            *GetCharacterId());
        return false;
    }

    // Fresh client-side request_id; the shared capture core does the rest.
    return DoWebcamCapture(FGuid::NewGuid().ToString(EGuidFormats::Digits), TEXT(""));
}

// ============================================================================
// DoWebcamCapture (CAM-07) — webcam capture core
//
// Shared by SendWebcamFrame (manual "b" key) and the server-initiated
// HandleCameraCaptureFromSubsystem (Webcam mode). Returns false (capturing
// nothing) if the webcam isn't started or the feed hasn't warmed up yet, so
// the server-initiated path can fall back to a scene capture. The caller must
// already hold the consent + bIsEncoding guards; on success EncodeAndEmitPixels
// takes over and sets bIsEncoding.
// ============================================================================

bool UEstuaryCameraComponent::DoWebcamCapture(const FString& RequestId, const FString& ContextText)
{
    check(IsInGameThread());

    if (!bWebcamOpen || !WebcamPlayer || !WebcamTexture || !WebcamBlitMID)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[CameraComponent] DoWebcamCapture: webcam not started — call StartWebcam first (char='%s')."),
            *GetCharacterId());
        return false;
    }

    // The device must have produced at least one frame (it warms up after OpenUrl).
    const FIntPoint Dim = WebcamPlayer->GetVideoTrackDimensions(INDEX_NONE, INDEX_NONE);
    if (!WebcamPlayer->IsReady() || Dim.X <= 0 || Dim.Y <= 0)
    {
        UE_LOG(LogEstuary, Warning,
            TEXT("[CameraComponent] DoWebcamCapture: webcam feed not ready yet (%dx%d) — "
                 "try again shortly (char='%s')."),
            Dim.X, Dim.Y, *GetCharacterId());
        return false;
    }

    // Ensure the readback RT exists and matches the current frame dimensions.
    if (!WebcamRT)
    {
        WebcamRT = NewObject<UTextureRenderTarget2D>(this, TEXT("EstuaryWebcamRT"));
        WebcamRT->RenderTargetFormat = RTF_RGBA8;
        WebcamRT->InitAutoFormat(Dim.X, Dim.Y);
        WebcamRT->UpdateResourceImmediate(true);
    }
    else if (WebcamRT->SizeX != Dim.X || WebcamRT->SizeY != Dim.Y)
    {
        WebcamRT->ResizeTarget(Dim.X, Dim.Y);
    }

    // Blit the latest media frame into the readback RT via the unlit blit material.
    UKismetRenderingLibrary::DrawMaterialToRenderTarget(this, WebcamRT, WebcamBlitMID);
    FlushRenderingCommands();

    // Read back BGRA pixels — identical path to the scene capture.
    TArray<FColor> Pixels;
    FTextureRenderTargetResource* RTResource = WebcamRT->GameThread_GetRenderTargetResource();
    const bool bReadOk = RTResource && RTResource->ReadPixels(Pixels);
    if (!bReadOk || Pixels.Num() == 0)
    {
        UE_LOG(LogEstuary, Error,
            TEXT("[CameraComponent] DoWebcamCapture: ReadPixels failed or returned empty — aborting."));
        return false;
    }

    // Reuse the shared encode + emit pipeline with the supplied request_id.
    EncodeAndEmitPixels(MoveTemp(Pixels), Dim.X, Dim.Y, RequestId, ContextText);
    return true;
}

// ============================================================================
// GetCharacterId — looks up owning AEstuaryCharacter (same pattern as CharacterComponent)
// ============================================================================

FString UEstuaryCameraComponent::GetCharacterId() const
{
    const AActor* Owner = GetOwner();
    if (!IsValid(Owner)) { return FString(); }

    if (const AEstuaryCharacter* CharActor = Cast<AEstuaryCharacter>(Owner))
    {
        return CharActor->CharacterId;
    }

    return FString();
}
