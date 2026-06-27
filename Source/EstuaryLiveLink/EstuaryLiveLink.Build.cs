// Copyright Estuary AI. All Rights Reserved.

using UnrealBuildTool;

public class EstuaryLiveLink : ModuleRules
{
    public EstuaryLiveLink(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        IWYUSupport = IWYUSupport.Full;

        // bUseUnity intentionally not set — accept UE default (unity build on)
        // following the EstuaryLiveKit pattern. Pitfall 23 commentary in
        // Estuary.Build.cs explains why MSVC PCH heap requires this on the
        // 30+ TUs across the plugin.

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "LiveLink",                // FLiveLinkSubjectKey, subject types
            "LiveLinkInterface",       // ILiveLinkSource, ILiveLinkClient,
                                       // ULiveLinkBasicRole, FLiveLinkBaseStaticData,
                                       // FLiveLinkBaseFrameData, *DataStruct wrappers
            "LiveLinkAnimationCore",   // forward-compat (no compile cost; future-proof)
            "Estuary",                 // one-way dep: LiveLink → core (Pitfall 7)
        });

        PrivateDependencyModuleNames.AddRange(new string[] { });
    }
}
