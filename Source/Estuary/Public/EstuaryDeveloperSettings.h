// EstuaryDeveloperSettings.h — UDeveloperSettings subclass exposed in Project Settings.
// Pitfall 23: #pragma once, IWYU, no CoreMinimal.h, no Engine.h.
//
// Access in C++:   UEstuaryDeveloperSettings::Get()->ServerUrl
// Access in BP:    Get Game Instance → Get Subsystem Estuary → (settings are read-only)
// Project Settings panel:  Project Settings → Plugins → Estuary
//
// API key storage (Option A — separate, untracked secret file, 2026-06-15):
//   The dev API key does NOT live on this class. It lives on UEstuarySecretSettings
//   (defined at the bottom of this file), which persists to Config/DefaultEstuarySecret.ini
//   — a local, untracked file, so it is per-developer / local-only and never committed.
//   This keeps Convai-style convenience (set once in Project Settings → Plugins →
//   "Estuary Secret"; survives editor restarts) WITHOUT writing the key into the shared,
//   version-controlled DefaultEstuaryPlugin.ini. GetEffectiveAuthToken() resolves the key
//   from that secret file, then falls back to the ESTUARY_API_KEY env var (CI / headless).
//   Note: config is still cooked into packaged builds, so do NOT ship a build with a real
//   key set — for production, pass the key at runtime via UEstuarySubsystem::Connect(Auth).

#pragma once

#include "Engine/DeveloperSettings.h"
#include "Containers/UnrealString.h"
#include "EstuaryTypes.h"  // EEstuaryFingerPolicy (UE-09)

#include "EstuaryDeveloperSettings.generated.h"

/**
 * Project-wide Estuary SDK configuration.
 *
 * Exposed in: Project Settings → Plugins → Estuary
 *
 * Config file: Config/DefaultEstuaryPlugin.ini
 * (under [/Script/Estuary.EstuaryDeveloperSettings])
 *
 * To read in C++:
 *   const UEstuaryDeveloperSettings* Settings = UEstuaryDeveloperSettings::Get();
 *   int32 Rate = Settings->AudioSampleRate;
 *
 * NOTE: the API key (AuthToken) is NOT on this class — it lives on UEstuarySecretSettings
 * (separate, local-only untracked Config/DefaultEstuarySecret.ini). See the "API key storage" note at
 * the top of this file. This class holds only shareable, committed settings.
 */
UCLASS(config = EstuaryPlugin, defaultconfig,
       meta = (DisplayName = "Estuary",
               ToolTip = "Project-wide Estuary SDK settings. Configure server URL, audio parameters, and camera quality."))
class ESTUARY_API UEstuaryDeveloperSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    /** Get the default CDO instance. Safe to call from any game-thread context.
     *  Returns the project's configured values (or defaults if not overridden). */
    static const UEstuaryDeveloperSettings* Get()
    {
        return GetDefault<UEstuaryDeveloperSettings>();
    }

    /** Project Settings panel category. "Plugins" places this under the Plugins section. */
    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

    /** Project Settings panel section name — shows as "Estuary" inside the Plugins category. */
    virtual FName GetSectionName()  const override { return TEXT("Estuary"); }

    // -----------------------------------------------------------------------
    // Connection settings
    // -----------------------------------------------------------------------

    /** Estuary gateway URL (Socket.IO over WebSocket).
     *  Default: https://api.estuary-ai.com (the hosted gateway; Socket.IO upgrades to wss://).
     *  Stored in DefaultEstuaryPlugin.ini. Override per-project in DefaultGame.ini. */
    UPROPERTY(config, EditAnywhere, BlueprintReadOnly, Category = "Connection",
              meta = (DisplayName = "Server URL",
                      ToolTip = "Estuary gateway URL (Socket.IO over WebSocket). Default: https://api.estuary-ai.com"))
    FString ServerUrl = TEXT("https://api.estuary-ai.com");

    /** Resolved developer API key for the authenticate payload.
     *  Resolution order:
     *    1. UEstuarySecretSettings::AuthToken (Project Settings -> Plugins -> "Estuary
     *       Secret"), if non-empty. Persisted to the local, untracked Config/DefaultEstuarySecret.ini,
     *       so it survives editor restarts WITHOUT being committed to version control.
     *    2. The ESTUARY_API_KEY environment variable (CI / headless / no-file fallback).
     *  Returns an empty string if neither source is set. For shipping, prefer passing the
     *  key explicitly via UEstuarySubsystem::Connect(Auth). */
    UFUNCTION(BlueprintPure, Category = "Estuary",
              meta = (ToolTip = "Resolved dev API key: the Estuary Secret Auth Token (local, untracked ini) if set, else the ESTUARY_API_KEY environment variable."))
    static FString GetEffectiveAuthToken();

    // -----------------------------------------------------------------------
    // Audio settings
    // -----------------------------------------------------------------------

    /** TTS playback sample rate in Hz, sent in the authenticate payload.
     *  Drives both the bot's LiveKit AudioSource rate and the ElevenLabs `pcm_<rate>`
     *  output_format. Default 24000 (higher fidelity). 16000 is supported and is the
     *  lowest rate the server's TTS providers will accept. NVIDIA Audio2Face accepts
     *  24000 (and other rates), so EnableAnimation no longer requires 16000.
     *  Mic capture rate is independently fixed at 16000 in EstuaryMicrophoneComponent
     *  (Deepgram requires it) — this setting only affects the TTS direction.
     *  Changing this at runtime requires a reconnect. */
    UPROPERTY(config, EditAnywhere, BlueprintReadOnly, Category = "Audio",
              meta = (DisplayName = "Audio Sample Rate",
                      ToolTip = "TTS playback sample rate (Hz) sent in authenticate payload. 24000 default (higher fidelity); 16000 also supported. Mic capture is always 16000 regardless.",
                      ClampMin = "8000", ClampMax = "48000"))
    int32 AudioSampleRate = 24000;

    /** Acoustic echo cancellation for LiveKit voice (WebRTC AEC3 via the LiveKit APM).
     *  The vendored livekit-client-sdk-cpp applies NO processing to custom-pushed mic PCM,
     *  and bot TTS renders through UE's own audio engine, so WebRTC's built-in AEC never
     *  engages on its own (Pitfall 27). When true, the SDK runs an explicit
     *  livekit::AudioProcessingModule loop: TTS playback feeds the far-end reference at
     *  audio-consumption pace and every mic window is echo-cancelled before publish.
     *  Applies to LiveKit voice mode only — WebSocket voice has no AEC (use the
     *  UEstuaryMicrophoneComponent SetSuppressed half-duplex guard there). */
    UPROPERTY(config, EditAnywhere, Category = "Audio",
              meta = (DisplayName = "LiveKit Echo Cancellation (AEC)",
                      ToolTip = "Run WebRTC AEC3 on the LiveKit mic path so the bot does not hear its own TTS through the speakers. LiveKit voice mode only."))
    bool bLiveKitEchoCancellation = true;

    /** AEC stream delay hint in milliseconds: estimated (render latency) + (capture latency)
     *  between the far-end reference feed and the echo appearing in mic frames.
     *  AEC3 also estimates delay internally, so this only needs to be roughly right.
     *  Default 50 ms suits typical Windows WASAPI output + mic-tick capture latency.
     *  Increase if echo persists with AEC enabled (e.g. Bluetooth audio devices). */
    UPROPERTY(config, EditAnywhere, Category = "Audio",
              meta = (DisplayName = "AEC Stream Delay (ms)",
                      ToolTip = "Estimated render+capture latency hint for AEC3. Default 50 ms; raise for high-latency outputs like Bluetooth.",
                      ClampMin = "0", ClampMax = "500",
                      EditCondition = "bLiveKitEchoCancellation"))
    int32 AecStreamDelayMs = 50;

    /** Enable ARKit-52 lipsync animation (bot_animation events).
     *  Requires server ENABLE_A2F = true. NVIDIA Audio2Face accepts the negotiated
     *  AudioSampleRate (16000 / 24000 / others) without coercion.
     *  Has no effect if the server does not have Audio2Face enabled. */
    UPROPERTY(config, EditAnywhere, Category = "Audio",
              meta = (DisplayName = "Enable Animation",
                      ToolTip = "Enable bot_animation lipsync stream. Requires server ENABLE_A2F=true."))
    bool bEnableAnimation = false;

    /** Enable bot_pose body-animation stream (Phase 4 — GestureSync).
     *  Requires server GestureSync inference + per-character animation_capabilities.body.enabled.
     *  Audio sample rate must be 16000 (HuBERT gate enforced server-side on the GestureSync path).
     *  Has no effect if the server does not have GestureSync inference enabled, or if the
     *  character lacks an animation_capabilities.body.enabled flag. */
    UPROPERTY(config, EditAnywhere, Category = "Audio",
              meta = (DisplayName = "Enable Body Animation",
                      ToolTip = "Enable bot_pose body animation stream. Requires server GestureSync inference + per-character animation_capabilities.body.enabled. Audio sample rate must be 16000 (HuBERT gate)."))
    bool bEnableBodyAnimation = false;

    // -----------------------------------------------------------------------
    // Body Animation Defaults (Phase 4 / UE-09)
    // -----------------------------------------------------------------------
    // Project-wide defaults for the GestureSync body-animation receiver. The
    // per-receiver UPROPERTY in the actor's Details panel takes precedence over
    // these project defaults (BP-set values always win — UE convention).

    /** Project-wide default for finger driving on UEstuaryBodyAnimationReceiver
     *  (UE-09). Per-receiver UPROPERTY override in the actor's Details
     *  panel takes precedence over this project-wide default. */
    UPROPERTY(config, EditAnywhere, Category = "Animation|Body",
              meta = (DisplayName = "Default Finger Policy",
                      ToolTip = "Project-wide default for finger driving on UEstuaryBodyAnimationReceiver. Driven (default) plays live GestureSync frames with bind-pose hold during 200 ms pre-buffer or gap > 100 ms. BindPose forces rest pose every frame (kiosk demos). IdleBreath behaves like Driven in v2.0 (additive idle math is a TODO Phase 4.x stub). Statue freezes fingers at last received frame (debug only — never default). Per-receiver UPROPERTY override in the actor's Details panel takes precedence."))
    EEstuaryFingerPolicy DefaultFingerPolicy = EEstuaryFingerPolicy::Driven;

    // -----------------------------------------------------------------------
    // Amplitude-driven jaw fallback defaults (Quick task 260428-365)
    // -----------------------------------------------------------------------
    // Project-wide defaults for the amplitude-driven jawOpen layer. Mirrors the
    // 5 UPROPERTYs on UEstuaryAnimationReceiver (Category Estuary|Animation|Amplitude).
    // Per-receiver UPROPERTY values override these defaults in v1 (the override is
    // "edit the BP component"); these settings exist so a project can set sensible
    // defaults once via Project Settings → Plugins → Estuary → Animation|Amplitude.

    UPROPERTY(config, EditAnywhere, Category = "Animation|Amplitude",
              meta = (DisplayName = "Enable Amplitude Jaw Fallback",
                      ToolTip = "Drives jawOpen from audio amplitude when A2F frames are absent (utterance start, end, gaps, cold-start). Set false to fully disable layer. Project-wide default; per-receiver UPROPERTY overrides this in Details panel."))
    bool bEnableAmplitudeJawFallback = true;

    UPROPERTY(config, EditAnywhere, Category = "Animation|Amplitude",
              meta = (DisplayName = "Amplitude Jaw Scale",
                      ClampMin = "0.0",
                      ClampMax = "2.0",
                      UIMin = "0.0",
                      UIMax = "1.5",
                      ToolTip = "Scales the amplitude-driven jaw weight (0.0 disables the layer entirely; 1.0 matches web SDK feel). Project-wide default; per-receiver UPROPERTY overrides this in Details panel."))
    float AmplitudeJawScale = 1.0f;

    UPROPERTY(config, EditAnywhere, Category = "Animation|Amplitude",
              meta = (DisplayName = "Amplitude Attack (ms)",
                      ClampMin = "1.0",
                      ClampMax = "200.0",
                      ToolTip = "Engine envelope follower attack time. Lower = jaw responds faster to loud transients. Default 10ms tracks speech crisply (engine uses peak detection). Project-wide default; per-receiver UPROPERTY overrides this in Details panel."))
    float AmplitudeAttackMs = 10.0f;

    UPROPERTY(config, EditAnywhere, Category = "Animation|Amplitude",
              meta = (DisplayName = "Amplitude Release (ms)",
                      ClampMin = "10.0",
                      ClampMax = "500.0",
                      ToolTip = "Engine envelope follower release time. Higher = jaw smooths over silence between syllables. Default 100ms matches web SDK perceptual feel. Project-wide default; per-receiver UPROPERTY overrides this in Details panel."))
    float AmplitudeReleaseMs = 100.0f;

    UPROPERTY(config, EditAnywhere, Category = "Animation|Amplitude",
              meta = (DisplayName = "A2F Crossfade (ms)",
                      ClampMin = "0.0",
                      ClampMax = "1000.0",
                      ToolTip = "Total ramp-in + ramp-out blend window between amplitude-only and full A2F. Half goes to fade-in (start of utterance), half to fade-out (end of utterance). Default 200ms; web SDK ships 150ms. Project-wide default; per-receiver UPROPERTY overrides this in Details panel."))
    float A2FCrossfadeMs = 200.0f;

    // -----------------------------------------------------------------------
    // Camera / VLM settings — used by Phase 7 UEstuaryCameraComponent
    // -----------------------------------------------------------------------

    /** JPEG quality for camera_image frames sent to the server (1-100).
     *  Lower = smaller payload, faster transmission; higher = better VLM recognition.
     *  Default 75 balances quality and bandwidth. */
    UPROPERTY(config, EditAnywhere, Category = "Camera",
              meta = (DisplayName = "JPEG Quality",
                      ToolTip = "JPEG compression quality for camera_image frames (1-100). Default 75.",
                      ClampMin = "1", ClampMax = "100"))
    int32 JpegQuality = 75;

    /** Maximum reconnect backoff delay in seconds.
     *  Exponential backoff caps at this value (2^n, capped at MaxBackoff).
     *  Default 30 seconds (matches UEstuarySubsystem implementation). */
    UPROPERTY(config, EditAnywhere, Category = "Connection",
              meta = (DisplayName = "Reconnect Max Backoff (seconds)",
                      ToolTip = "Exponential backoff ceiling for reconnect attempts (seconds). Default 30.",
                      ClampMin = "1", ClampMax = "300"))
    int32 ReconnectMaxBackoffSeconds = 30;

    /** Force WebSocket-only voice transport — no LiveKit token requests, no
     *  LiveKit room joins, even if the gateway pre-bakes a `livekitToken` into
     *  session_info. Use during development/debugging to avoid burning LiveKit
     *  credits while iterating. The mic still streams via Socket.IO `stream_audio`
     *  events; bot TTS still arrives via `bot_voice` PCM16 frames. Default false
     *  preserves the v1 LiveKit-when-available behavior for shipping projects.
     *
     *  When true:
     *    - `UEstuarySubsystem::RequestLiveKitToken()` becomes a no-op (logs Verbose).
     *    - Any `livekit_token` server event is dropped before
     *      `HandleLiveKitTokenReceived` would try to join a room. */
    UPROPERTY(config, EditAnywhere, Category = "Connection",
              meta = (DisplayName = "Disable LiveKit (WebSocket-only voice)",
                      ToolTip = "Force WebSocket voice. Skips all LiveKit token requests + auto-joins. Use during dev/debug to avoid LiveKit credits."))
    bool bDisableLiveKit = false;
};

// ---------------------------------------------------------------------------
// UEstuarySecretSettings — the API key ONLY, in a SEPARATE, untracked config file.
// ---------------------------------------------------------------------------
//
// Why a second settings class: UE writes ALL of a class's config UPROPERTYs to the single
// .ini named by its UCLASS(config=...). To keep the secret OUT of the shared, version-
// controlled DefaultEstuaryPlugin.ini (which holds settings the team DOES commit), the key
// gets its own class whose config file — Config/DefaultEstuarySecret.ini — is kept local and
// untracked, and is therefore per-developer / local-only.
//
// Convai-parity UX is preserved: set the key once in Project Settings -> Plugins ->
// "Estuary Secret"; it persists across editor restarts. Unlike Convai, it is NOT committed
// and NOT written into a shared config. (config IS still cooked into packaged builds, so do
// not ship a build with a real key set — use UEstuarySubsystem::Connect(Auth) for production.)

/**
 * Local, untracked developer-secret settings. Holds ONLY the API key.
 * Config file: Config/DefaultEstuarySecret.ini (local-only, untracked — never commit).
 */
UCLASS(config = EstuarySecret, defaultconfig,
       meta = (DisplayName = "Estuary Secret",
               ToolTip = "Local-only developer API key. Persists to the local, untracked Config/DefaultEstuarySecret.ini — never committed to version control."))
class ESTUARY_API UEstuarySecretSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    /** CDO accessor — safe from any game-thread context. */
    static const UEstuarySecretSettings* Get()
    {
        return GetDefault<UEstuarySecretSettings>();
    }

    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
    virtual FName GetSectionName()  const override { return TEXT("Estuary Secret"); }

    /** Developer API key. Persisted to the LOCAL, untracked Config/DefaultEstuarySecret.ini,
     *  so it survives editor restarts without being committed to version control. Read via
     *  UEstuaryDeveloperSettings::GetEffectiveAuthToken(). For production, pass the key at
     *  runtime via UEstuarySubsystem::Connect(Auth) instead. */
    UPROPERTY(config, EditAnywhere, BlueprintReadOnly, Category = "Connection",
              meta = (DisplayName = "Auth Token (local-only — saved to untracked DefaultEstuarySecret.ini)",
                      ToolTip = "Developer API key. Saved to a local, untracked Config/DefaultEstuarySecret.ini (never committed). Survives editor restarts. Use UEstuarySubsystem::Connect(Auth) for production."))
    FString AuthToken;
};
