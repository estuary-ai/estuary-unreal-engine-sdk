// EstuaryCameraComponent.h — USceneComponent subclass for scene capture, JPEG encode, and emit.
//
// Pitfall 23: #pragma once, IWYU, no CoreMinimal.h, no Engine.h in public header.
// Pitfall 7:  Zero LiveKit references.
// Pitfall 1:  All game-thread guards via check(IsInGameThread()) in implementation.
// CAM-01: USceneComponent subclass, attachable.
// CAM-02: JPEG encode off game thread via AsyncTask(AnyThread).
// CAM-03: Server-initiated camera_capture routes through subsystem to this component.
// CAM-04: Consent gate — bHasUserConsent=false default; no capture without SetCameraConsent(true).
// CAM-05: AcquireCameraPermission Blueprint node available via UEstuaryPermissionAction (Phase 5).

#pragma once

#include "CoreTypes.h"
#include "UObject/ObjectMacros.h"
#include "UObject/SoftObjectPtr.h"
#include "Components/SceneComponent.h"
#include "Templates/Atomic.h"
#include "EstuaryTypes.h"

#include "EstuaryCameraComponent.generated.h"

// Forward declarations — full headers in .cpp only (IWYU / Pitfall 23).
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
// Webcam (device-camera) mode — UE Media Framework + a blit material.
class UMediaPlayer;
class UMediaTexture;
class UMaterialInterface;
class UMaterialInstanceDynamic;

/**
 * UEstuaryCameraComponent — attachable scene-capture component for Estuary camera/VLM.
 *
 * Attach this to an AEstuaryCharacter (or any Actor) in the Details panel.
 * It owns a child USceneCaptureComponent2D and UTextureRenderTarget2D.
 *
 * Usage flow:
 *   1. Call SetCameraConsent(true) after the user grants camera permission
 *      (use UEstuaryPermissionAction::AcquireCameraPermission for the OS dialog).
 *   2. Call SendCameraFrame() to capture, JPEG-encode (off game thread), and emit camera_image.
 *   3. The subsystem matches the bot_response to the pending request_id and fires
 *      UEstuaryCharacterComponent::OnVisionResponse.
 *
 * Thread safety: all public methods must be called on the game thread.
 *   JPEG encode runs on ENamedThreads::AnyThread; result marshals back to the game thread.
 *   TAtomic<bool> bIsEncoding prevents overlapping encodes.
 */
UCLASS(ClassGroup=(Estuary),
       meta=(BlueprintSpawnableComponent,
             DisplayName="Estuary Camera Component",
             ToolTip="Captures the Unreal scene via a render target, JPEG-encodes off-thread, and emits camera_image. Call SetCameraConsent(true) before SendCameraFrame."))
class ESTUARY_API UEstuaryCameraComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UEstuaryCameraComponent();

    // -----------------------------------------------------------------------
    // Blueprint-callable API (CAM-01, CAM-03, CAM-04, CAM-05)
    // -----------------------------------------------------------------------

    /** Grant or revoke consent for scene capture. Default is false (no capture).
     *  No camera_image is ever transmitted until SetCameraConsent(true) has been called.
     *  Call after UEstuaryPermissionAction::AcquireCameraPermission fires Granted. */
    UFUNCTION(BlueprintCallable, Category="Estuary",
              meta=(DisplayName="Set Camera Consent",
                    ToolTip="Grant or revoke user consent for scene capture. Default false. Required before SendCameraFrame will transmit."))
    void SetCameraConsent(bool bGranted);

    /** Capture the current scene frame, JPEG-encode on a background task, and emit camera_image.
     *
     *  Returns false immediately (no capture) if:
     *    (a) bHasUserConsent is false — logs a Warning.
     *    (b) A previous encode is still in flight (bIsEncoding) — logs Verbose.
     *
     *  Returns true when the capture pipeline was successfully initiated.
     *  The actual emit happens asynchronously after encode completes on AnyThread. */
    UFUNCTION(BlueprintCallable, Category="Estuary",
              meta=(DisplayName="Send Camera Frame",
                    ToolTip="Capture scene, JPEG-encode off game-thread, emit camera_image. Returns false without consent or during an active encode."))
    bool SendCameraFrame();

    /** Resize the capture render target. Returns false if Width or Height <= 0.
     *  Default size is 512x512 (set in InitializeComponent). */
    UFUNCTION(BlueprintCallable, Category="Estuary",
              meta=(DisplayName="Set Render Target Size",
                    ToolTip="Resize the capture render target. Default 512x512. Returns false for non-positive dimensions."))
    bool SetRenderTargetSize(int32 Width, int32 Height);

    // -----------------------------------------------------------------------
    // Webcam (device-camera) mode — UE Media Framework (CAM-07)
    //
    // Mirrors the scene-capture path (SendCameraFrame) but sources pixels from a
    // physical capture device instead of the rendered scene. Both paths share the
    // same JPEG-encode → camera_image emit pipeline (EncodeAndEmitPixels).
    //
    // Usage flow:
    //   1. SetCameraConsent(true)  — same consent gate as scene capture.
    //   2. StartWebcam()           — opens the device; the feed needs a moment to warm up.
    //   3. SendWebcamFrame()       — grabs the latest decoded frame and emits camera_image.
    //   4. StopWebcam()            — closes the device (also called automatically in EndPlay).
    // -----------------------------------------------------------------------

    /** Open a physical video-capture device (webcam) via the Media Framework.
     *  DeviceIndex selects among enumerated devices (0 = first/default).
     *  Returns false if no device is found or the media objects can't be created.
     *  The device feed warms up asynchronously — SendWebcamFrame only succeeds once
     *  the first frame has decoded (it logs a Warning and no-ops until then). */
    UFUNCTION(BlueprintCallable, Category="Estuary",
              meta=(DisplayName="Start Webcam",
                    ToolTip="Open a webcam/video-capture device via the Media Framework. DeviceIndex 0 = default. Feed warms up asynchronously."))
    bool StartWebcam(int32 DeviceIndex = 0);

    /** Close the active webcam device and release the media player/texture. Safe to call when not started. */
    UFUNCTION(BlueprintCallable, Category="Estuary",
              meta=(DisplayName="Stop Webcam",
                    ToolTip="Close the active webcam device. Safe to call if the webcam was never started."))
    void StopWebcam();

    /** Capture the latest webcam frame, JPEG-encode off the game thread, and emit camera_image.
     *
     *  Returns false immediately (no capture) if:
     *    (a) bHasUserConsent is false — logs a Warning.
     *    (b) A previous encode is still in flight (bIsEncoding) — logs Verbose.
     *    (c) StartWebcam hasn't been called / the device isn't open — logs a Warning.
     *    (d) No frame has decoded yet (device still warming up) — logs a Warning.
     *
     *  This is the webcam analogue of SendCameraFrame and shares its emit pipeline. */
    UFUNCTION(BlueprintCallable, Category="Estuary",
              meta=(DisplayName="Send Webcam Frame",
                    ToolTip="Grab the latest webcam frame, JPEG-encode off game-thread, emit camera_image. Requires StartWebcam + consent + a warmed-up feed."))
    bool SendWebcamFrame();

    // -----------------------------------------------------------------------
    // Capture source mode — selects which camera feeds the VLM (CAM-07)
    // -----------------------------------------------------------------------

    /** Current capture source. Governs the server-initiated camera_capture path
     *  (HandleCameraCaptureFromSubsystem): RenderedScene → scene capture,
     *  Webcam → device capture (with scene fallback if the feed isn't ready).
     *  Default RenderedScene. Set via SetCaptureSource or the subsystem toggle. */
    UPROPERTY(BlueprintReadOnly, Category="Estuary|Camera",
              meta=(DisplayName="Capture Source",
                    ToolTip="Which camera feeds the VLM. RenderedScene = the 3D scene; Webcam = the device camera."))
    EEstuaryCaptureSource CaptureSource = EEstuaryCaptureSource::RenderedScene;

    /** Select the capture source. Switching to Webcam auto-starts the device so a
     *  subsequent capture has a live feed ready. */
    UFUNCTION(BlueprintCallable, Category="Estuary",
              meta=(DisplayName="Set Capture Source",
                    ToolTip="Choose Rendered Scene or Webcam as the capture source. Webcam mode auto-starts the device."))
    void SetCaptureSource(EEstuaryCaptureSource NewSource);

    /** True when the current capture source is the webcam. */
    UFUNCTION(BlueprintPure, Category="Estuary",
              meta=(DisplayName="Is Webcam Mode"))
    bool IsWebcamMode() const { return CaptureSource == EEstuaryCaptureSource::Webcam; }

    // -----------------------------------------------------------------------
    // Internal dispatch — called by UEstuarySubsystem when camera_capture arrives.
    // NOT BlueprintCallable — subsystem routing only.
    // check(IsInGameThread()) enforced inside.
    // -----------------------------------------------------------------------

    /** Called by UEstuarySubsystem::HandleCameraCapture on the game thread.
     *  Checks consent + bIsEncoding guard, then calls DoCapture with the server's request_id. */
    void HandleCameraCaptureFromSubsystem(const FEstuaryCameraCapture& Capture);

    // -----------------------------------------------------------------------
    // Registry helper — used by subsystem to look up owning character.
    // Returns the CharacterId of the owning AEstuaryCharacter, or empty string.
    // -----------------------------------------------------------------------
    FString GetCharacterId() const;

    // -----------------------------------------------------------------------
    // USceneComponent overrides
    // -----------------------------------------------------------------------

    /** Creates the render target (512x512 RGBA8) and attaches SceneCapture to this component. */
    virtual void InitializeComponent() override;

    /** Registers this component with UEstuarySubsystem's camera component registry. */
    virtual void BeginPlay() override;

    /** Unregisters from UEstuarySubsystem's camera component registry. */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    // -----------------------------------------------------------------------
    // Configuration UPROPERTYs — editable in the Details panel
    // -----------------------------------------------------------------------

    /** Auto-created child USceneCaptureComponent2D. Created in constructor as a subobject.
     *  bCaptureEveryFrame and bCaptureOnMovement are disabled — we call CaptureScene() manually. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Estuary|Camera",
              meta=(DisplayName="Scene Capture",
                    ToolTip="Auto-created child USceneCaptureComponent2D. Manual capture only — use SendCameraFrame()."))
    USceneCaptureComponent2D* SceneCapture = nullptr;

    /** Auto-created UTextureRenderTarget2D. Created in InitializeComponent (512x512 RGBA8).
     *  Resize via SetRenderTargetSize(). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Estuary|Camera",
              meta=(DisplayName="Render Target",
                    ToolTip="Auto-created 512x512 RGBA8 render target. Resize via SetRenderTargetSize(Width, Height)."))
    UTextureRenderTarget2D* RenderTarget = nullptr;

    /** JPEG compression quality (1-100). Default 75.
     *  Higher values produce larger payloads. 75 is a good latency/quality balance. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Estuary|Camera",
              meta=(DisplayName="JPEG Quality",
                    ToolTip="JPEG compression quality 1-100. Default 75. Higher values produce larger payloads and longer encode times.",
                    ClampMin="1", ClampMax="100"))
    int32 JpegQuality = 75;

    /** User consent flag. Default false. Set via SetCameraConsent().
     *  No capture or emit path executes until this is true (T-07-04, CAM-04). */
    UPROPERTY(BlueprintReadOnly, Category="Estuary|Camera",
              meta=(DisplayName="Has User Consent",
                    ToolTip="True after SetCameraConsent(true). No scene capture executes without consent."))
    bool bHasUserConsent = false;

    // -----------------------------------------------------------------------
    // Webcam-mode configuration UPROPERTYs
    // -----------------------------------------------------------------------

    /** Material used to blit the webcam UMediaTexture into a CPU-readable render target.
     *  Must be an unlit material with a "MediaTex" TextureSampleParameter2D wired to Emissive.
     *  If unset, StartWebcam falls back to loading /Game/Materials/M_EstuaryWebcamBlit. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Estuary|Webcam",
              meta=(DisplayName="Webcam Blit Material",
                    ToolTip="Unlit material with a 'MediaTex' texture parameter, used to copy the webcam feed into a readable render target. Defaults to /Game/Materials/M_EstuaryWebcamBlit."))
    TSoftObjectPtr<UMaterialInterface> WebcamBlitMaterial;

    /** Media player driving the open webcam device. Created by StartWebcam. */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Estuary|Webcam",
              meta=(DisplayName="Webcam Media Player"))
    UMediaPlayer* WebcamPlayer = nullptr;

    /** Media texture receiving decoded webcam frames. Created by StartWebcam. */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Estuary|Webcam",
              meta=(DisplayName="Webcam Media Texture"))
    UMediaTexture* WebcamTexture = nullptr;

    /** Separate render target for webcam readback (kept distinct from the scene RT). */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Estuary|Webcam",
              meta=(DisplayName="Webcam Render Target"))
    UTextureRenderTarget2D* WebcamRT = nullptr;

private:
    /** Dynamic instance of WebcamBlitMaterial with its MediaTex param bound to WebcamTexture. */
    UPROPERTY(Transient)
    UMaterialInstanceDynamic* WebcamBlitMID = nullptr;

    /** True between a successful StartWebcam and StopWebcam. */
    bool bWebcamOpen = false;
    /** Atomic encode-in-flight guard.
     *  Set true before AsyncTask(AnyThread), false in the game-thread marshal-back.
     *  Prevents overlapping encodes from stacking (T-07-05 / CAM-02). */
    TAtomic<bool> bIsEncoding{false};

    /** True between RegisterCameraComponent and UnregisterCameraComponent. */
    bool bIsRegistered = false;

    /** Scene-capture path: render the scene into RenderTarget, read it back, then hand off
     *  to EncodeAndEmitPixels. Must be called on the game thread.
     *  Caller is responsible for consent + bIsEncoding guards before calling DoCapture. */
    void DoCapture(const FString& RequestId, const FString& ContextText);

    /** Shared tail of both the scene-capture and webcam paths: marks the encode in flight,
     *  JPEG-encodes the BGRA pixels off the game thread, and emits camera_image on the game
     *  thread. Takes ownership of Pixels (moved into the background task).
     *  Caller must have already passed the consent + bIsEncoding guards. */
    void EncodeAndEmitPixels(TArray<FColor>&& Pixels, int32 Width, int32 Height,
                             const FString& RequestId, const FString& ContextText);

    /** Webcam capture core, shared by SendWebcamFrame and the server-initiated
     *  HandleCameraCaptureFromSubsystem (when in Webcam mode). Returns false
     *  (capturing nothing) if the webcam isn't started or the feed isn't ready.
     *  Caller must already hold the consent + bIsEncoding guards. */
    bool DoWebcamCapture(const FString& RequestId, const FString& ContextText);
};
