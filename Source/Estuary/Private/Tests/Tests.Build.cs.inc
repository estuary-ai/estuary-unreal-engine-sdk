// Tests.Build.cs.inc — Notes for integrating Estuary automation tests.
//
// This file is NOT a real Build.cs or module definition. It is a documentation
// fragment explaining how the test files in this directory are compiled into the
// Estuary module for use with UE's Session Frontend automation runner.
//
// ============================================================================
// HOW UE AUTOMATION TESTS WORK WITH THIS PLUGIN
// ============================================================================
//
// 1. Test files (*.cpp in this Tests/ folder) are compiled directly as part of the
//    Estuary module — they are NOT in a separate test module. This is the standard
//    UE pattern for plugin unit tests that require the plugin's own types (UObjects,
//    plain C++ structs).
//
// 2. The #if WITH_DEV_AUTOMATION_TESTS / #endif guard wrapping every test file
//    ensures the test code is stripped from Shipping builds. WITH_DEV_AUTOMATION_TESTS
//    is defined by the UE build system when UBT detects a Development or Debug
//    configuration AND the project has automation tests enabled.
//
// 3. In Estuary.Build.cs, add the following to pick up the test files:
//
//       if (Target.Configuration == UnrealTargetConfiguration.Development ||
//           Target.Configuration == UnrealTargetConfiguration.DebugGame)
//       {
//           PrivateDependencyModuleNames.AddRange(new string[]
//           {
//               "AutomationController",   // For IMPLEMENT_SIMPLE_AUTOMATION_TEST
//           });
//       }
//
//    The test .cpp files are already under Private/ so UBT discovers them automatically
//    via the module's PrivateIncludePaths glob. No explicit file listing is needed.
//
// 4. Tests run in the UE editor via:
//       Window > Session Frontend > Automation tab
//    Filter by "Estuary.Audio" or "Estuary.Animation" to find the plugin tests.
//
// 5. CommandLine automation (CI / GitHub Actions) — run via RunUAT:
//
//       RunUAT.bat RunUnrealTests
//         -project="<path>.uproject"
//         -filter="Estuary."
//         -outputdir="<results-dir>"
//
//    See .github/workflows/ue55-build.yml for the CI scaffold that wraps this.
//
// ============================================================================
// ADDING NEW TESTS
// ============================================================================
//
// 1. Create a new .cpp file in Source/Estuary/Private/Tests/.
// 2. Wrap the entire file in #if WITH_DEV_AUTOMATION_TESTS / #endif.
// 3. Use IMPLEMENT_SIMPLE_AUTOMATION_TEST for stateless tests.
// 4. Use DEFINE_LATENT_AUTOMATION_COMMAND if the test requires async game-thread ticks.
// 5. Follow the naming convention: "Estuary.<Subsystem>.<TestName>"
//    (e.g., "Estuary.Audio.RingBuffer.WrapAround").
// 6. Assign EAutomationTestFlags::SmokeFilter for fast tests that should run in CI;
//    EAutomationTestFlags::ProductFilter for longer integration-style tests.
//
// ============================================================================
