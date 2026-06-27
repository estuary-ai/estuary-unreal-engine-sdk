// Copyright Estuary AI. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class EstuaryLiveKit : ModuleRules
{
    public EstuaryLiveKit(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // bUseUnity intentionally not set — accept UE default (unity build on) so MSVC
        // doesn't exhaust PCH heap on the 30+ TUs in this plugin.

        IWYUSupport = IWYUSupport.Full;

        // SDK throws std::runtime_error from FFI errors / captureFrame timeout.
        bEnableExceptions = true;
        // SDK internals use dynamic_cast.
        bUseRTTI = true;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Projects",  // IPluginManager — used to resolve Plugin->GetBaseDir() for explicit DLL load.
            "Estuary",   // One-way dependency: LiveKit depends on core, not the reverse.
        });

        PrivateDependencyModuleNames.AddRange(new string[] { });

        // Detect the livekit-client-sdk-cpp prebuilts under ThirdParty/.
        // Run ThirdParty/Setup.ps1 (Windows) or ThirdParty/Setup.sh (macOS/Linux) once
        // per workstation to fetch the v0.3.4 prebuilt for the current host triple.
        string ThirdPartyPath = Path.Combine(ModuleDirectory, "..", "..", "ThirdParty", "livekit-client-sdk-cpp");
        bool bHasLiveKit = Directory.Exists(ThirdPartyPath);

        if (bHasLiveKit)
        {
            PublicDefinitions.Add("WITH_ESTUARY_LIVEKIT=1");

            string IncludePath = Path.Combine(ThirdPartyPath, "include");
            PublicIncludePaths.Add(IncludePath);

            // Flat lib/ and bin/ layout — matches the prebuilt zip/tarball exactly.
            // Setup.{ps1,sh} flatten the inner "livekit-sdk-{triple}-{ver}/" dir for us.
            string LibPath = Path.Combine(ThirdPartyPath, "lib");
            string BinPath = Path.Combine(ThirdPartyPath, "bin");

            if (Target.Platform == UnrealTargetPlatform.Win64)
            {
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "livekit.lib"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "livekit_ffi.dll.lib"));

                RuntimeDependencies.Add(Path.Combine(BinPath, "livekit.dll"));
                RuntimeDependencies.Add(Path.Combine(BinPath, "livekit_ffi.dll"));
                PublicDelayLoadDLLs.Add("livekit.dll");
                PublicDelayLoadDLLs.Add("livekit_ffi.dll");
            }
            else if (Target.Platform == UnrealTargetPlatform.Mac)
            {
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "liblivekit.dylib"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "liblivekit_ffi.dylib"));

                RuntimeDependencies.Add(Path.Combine(LibPath, "liblivekit.dylib"));
                RuntimeDependencies.Add(Path.Combine(LibPath, "liblivekit_ffi.dylib"));

                PublicFrameworks.AddRange(new string[]
                {
                    "AudioToolbox",
                    "AVFoundation",
                    "CoreAudio",
                    "CoreMedia",
                    "CoreVideo",
                    "Foundation",
                    "Security",
                    "SystemConfiguration",
                });
            }
            else if (Target.Platform == UnrealTargetPlatform.Linux)
            {
                // Linux is not a v1 ship target but supported for editor-host development on WSL.
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "liblivekit.so"));
                PublicAdditionalLibraries.Add(Path.Combine(LibPath, "liblivekit_ffi.so"));

                RuntimeDependencies.Add(Path.Combine(LibPath, "liblivekit.so"));
                RuntimeDependencies.Add(Path.Combine(LibPath, "liblivekit_ffi.so"));
            }
            // iOS and Android are not in v1 scope for LiveKit.
            // WhitelistPlatforms in .uplugin prevents this module from loading on mobile.
        }
        else
        {
            PublicDefinitions.Add("WITH_ESTUARY_LIVEKIT=0");
        }
    }
}
