// Copyright Estuary AI. All Rights Reserved.
// SCAF-02: PCHUsage=UseExplicitOrSharedPCHs, IWYUSupport=Full (unity build left at UE default).
// True drop-in: WITH_ESTUARY_SOCKETIO and WITH_ESTUARY_RAI gate sibling-plugin deps.
// Same pattern as EstuaryLiveKit: probe Plugins/<DepName> at build time and either
// link the dep + define WITH_*=1, or fall back to stub mode with WITH_*=0. The editor
// auto-installer prompts the user to install missing deps on first launch.

using UnrealBuildTool;
using System.IO;

public class Estuary : ModuleRules
{
    public Estuary(ReadOnlyTargetRules Target) : base(Target)
    {
        // SCAF-02: Explicit PCH — every file includes only what it uses.
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // bUseUnity intentionally not set — accept UE default (unity build on) so MSVC
        // doesn't exhaust PCH heap on the 30+ TUs in this plugin. Convai and Inworld
        // both ship with unity build enabled. SCAF-02's "no unity for strict IWYU" was
        // over-strict for a plugin this size on UE 5.7.

        // SCAF-02: Full IWYU enforcement (UE 5.5+ property).
        IWYUSupport = IWYUSupport.Full;

        PublicIncludePaths.AddRange(new string[] { });
        PrivateIncludePaths.AddRange(new string[] { });

        // Core UE modules always required by a Runtime plugin (no sibling-plugin deps).
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "AudioCaptureCore", // Audio::FAudioCapture — Windows mic fallback when RAI is absent
            "AudioExtensions",  // UE 5.6+: provides Audio::IProxyData / FProxyDataInitParams (AudioProxyInitializer.h)
            "AudioMixer",
            "Core",
            "CoreUObject",
            "DeveloperSettings",
            "Engine",
            "InputCore",
            "Json",
            "JsonUtilities",
            "Projects",
            "SignalProcessing",
            "SoundUtilities",
            "WebSockets",
            // AndroidPermission — UE built-in module (no sibling install required).
            // Provides UAndroidPermissionFunctionLibrary for runtime RECORD_AUDIO + CAMERA
            // dialogs. Headers exist cross-platform; functions are no-ops on non-Android.
            "AndroidPermission",
        });

        // Sibling-plugin path: PluginDirectory == .../Plugins/Estuary, so siblings live at
        // PluginDirectory/../<DepName>/<DepName>.uplugin.
        string PluginsRoot = Path.Combine(PluginDirectory, "..");

        // ----- WITH_ESTUARY_SOCKETIO -------------------------------------------------------
        // getnamo/SocketIOClient-Unreal v2.11.0 — required for Socket.IO v4 transport.
        // When absent, EstuarySocketIOBridge compiles to a no-op stub (warns + early-returns).
        string SocketIOPluginPath = Path.Combine(PluginsRoot, "SocketIOClient");
        bool bHasSocketIO = Directory.Exists(SocketIOPluginPath)
            && File.Exists(Path.Combine(SocketIOPluginPath, "SocketIOClient.uplugin"));

        if (bHasSocketIO)
        {
            PublicDefinitions.Add("WITH_ESTUARY_SOCKETIO=1");
            PublicDependencyModuleNames.AddRange(new string[] { "SocketIOClient", "SIOJson" });
        }
        else
        {
            PublicDefinitions.Add("WITH_ESTUARY_SOCKETIO=0");
            // No module deps added — bridge compiles as stub. Editor auto-installer prompts.
        }

        // ----- WITH_ESTUARY_LIVEKIT --------------------------------------------------------
        // EstuaryLiveKitBridge.h (in core's Public/) uses #if WITH_ESTUARY_LIVEKIT, so the
        // macro must be defined when compiling the core module. The EstuaryLiveKit module
        // also defines this in its own Build.cs based on the same probe — keep both in sync.
        string LiveKitThirdPartyPath = Path.Combine(PluginDirectory, "ThirdParty", "livekit-client-sdk-cpp");
        bool bHasLiveKit = Directory.Exists(LiveKitThirdPartyPath);
        PublicDefinitions.Add(bHasLiveKit ? "WITH_ESTUARY_LIVEKIT=1" : "WITH_ESTUARY_LIVEKIT=0");

        // ----- WITH_ESTUARY_RAI ------------------------------------------------------------
        // gtreshchev/RuntimeAudioImporter (Fab or OSS) — cross-platform mic capture.
        // When absent, UEstuaryMicrophoneComponent compiles without UCapturableSoundWave;
        // StartListening() is a no-op (warns) and the SPSC queue stays empty.
        string RAIPluginPath = Path.Combine(PluginsRoot, "RuntimeAudioImporter");
        bool bHasRAI = Directory.Exists(RAIPluginPath)
            && File.Exists(Path.Combine(RAIPluginPath, "RuntimeAudioImporter.uplugin"));

        if (bHasRAI)
        {
            PublicDefinitions.Add("WITH_ESTUARY_RAI=1");
            PublicDependencyModuleNames.Add("RuntimeAudioImporter");
        }
        else
        {
            PublicDefinitions.Add("WITH_ESTUARY_RAI=0");
            // No module deps added — mic component compiles without RAI types.
        }

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "RenderCore",
            "RHI",
            "ImageWrapper",
            // Device-camera (webcam) capture via UE Media Framework:
            //   MediaAssets — UMediaPlayer / UMediaTexture / UMediaSource.
            //   MediaUtils  — MediaCaptureSupport::EnumerateVideoCaptureDevices (FMediaCaptureDeviceInfo).
            // Platform players (WmfMedia on Windows, AvfMedia on Mac, AndroidMedia on Android) are
            // loaded by the media framework at runtime — no direct build dependency on them here.
            "MediaAssets",
            "MediaUtils",
        });

        // NOTE: The optional WebRTC compile-time define is scoped to the
        // EstuaryLK module only (Pitfall 7: two-module split keeps core clean).
        // Core has zero dependency on the optional WebRTC wrapper.

        // iOS: AVFoundation framework for AVAudioSession category management (Pitfall 8).
        // PublicFrameworks links Apple system frameworks; PrivateDependencyModuleNames
        // is for UE modules — do NOT use PrivateDependencyModuleNames for "AVFoundation".
        if (Target.Platform == UnrealTargetPlatform.IOS)
        {
            PublicFrameworks.AddRange(new string[]
            {
                "AVFoundation",
                "AudioToolbox",
            });
        }
    }
}
