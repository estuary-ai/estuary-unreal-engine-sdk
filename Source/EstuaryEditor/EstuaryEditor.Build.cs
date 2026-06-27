// Copyright Estuary AI. All Rights Reserved.
// SCAF-02: PCHUsage=UseExplicitOrSharedPCHs, IWYUSupport=Full (unity build left at UE default).

using UnrealBuildTool;

public class EstuaryEditor : ModuleRules
{
    public EstuaryEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        // SCAF-02: Explicit PCH.
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // bUseUnity intentionally not set — accept UE default (unity build on) so MSVC
        // doesn't exhaust PCH heap on the 30+ TUs in this plugin. Convai and Inworld
        // both ship with unity build enabled. SCAF-02's "no unity for strict IWYU" was
        // over-strict for a plugin this size on UE 5.7.

        // SCAF-02: Full IWYU enforcement (UE 5.5+ property).
        IWYUSupport = IWYUSupport.Full;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "UnrealEd",
            "Slate",
            "SlateCore",
            "EditorFramework",   // Required for FSlateNotificationManager (dependency notifier — API-06)
            "EditorSubsystem",
            "Estuary",   // Editor module depends on runtime core.
        });

        PrivateDependencyModuleNames.AddRange(new string[] { });

        // Editor module is Win64/Mac only — this is enforced by WhitelistPlatforms
        // in Estuary.uplugin rather than here in Build.cs.
        // Phase 9: EstuaryEditor will host the dependency-notifier panel (Pitfall 7
        // mitigation UI: surfaces missing SocketIOClient + optional LiveKit status).
    }
}
