// EstuaryBodyBoneRemapTests.cpp — DataTable load + lookup tests for the
// GestureSync (SMPL-X) → MetaHuman bone-name remap (UE-04).
//
// HOW TO RUN:
//   1. Open your UE 5.6+ project in the editor (DataTable load needs the asset registry).
//   2. Window > Session Frontend > Automation tab.
//   3. Filter by "Estuary.Body.BoneRemap".
//   4. Select tests below and click "Run Tests".
//
// Phase 4 (UE-04 unit tests). Mirror of EstuaryAmplitudeEnvelopeTests.cpp
// scaffold + DataTable load idioms from EstuaryAnimationFrameBufferTests.cpp.
// Pitfall 23: IWYU only.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/DataTable.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "EstuaryBodyBoneRemap.h"

namespace EstuaryBodyBoneRemapTestHelpers
{
    /** Load the bundled DT_GestureSyncBoneRemap_Identity DataTable.
     *  Asset path mirrors face's /Estuary/DataTables/DT_ArkitRemap_Identity layout. */
    static UDataTable* LoadIdentityTable()
    {
        return LoadObject<UDataTable>(
            nullptr,
            TEXT("/Estuary/DataTables/DT_GestureSyncBoneRemap_Identity.DT_GestureSyncBoneRemap_Identity"));
    }

    /** Hard-coded MetaHuman skeleton bone-name allow-list (Assumption A4 mitigation).
     *  Every UEBoneName in DT_GestureSyncBoneRemap_Identity must appear in this set —
     *  guards against silent SMPLX_TO_METAHUMAN dict drift in the codegen script. */
    static TSet<FName> MakeMetaHumanBoneAllowList()
    {
        return TSet<FName>{
            FName("pelvis"), FName("thigh_l"), FName("thigh_r"),
            FName("spine_01"), FName("calf_l"), FName("calf_r"),
            FName("spine_03"), FName("foot_l"), FName("foot_r"),
            FName("spine_05"), FName("ball_l"), FName("ball_r"),
            FName("neck_01"), FName("clavicle_l"), FName("clavicle_r"),
            FName("head"),
            FName("upperarm_l"), FName("upperarm_r"),
            FName("lowerarm_l"), FName("lowerarm_r"),
            FName("hand_l"), FName("hand_r"),
            // Left fingers
            FName("index_01_l"),  FName("index_02_l"),  FName("index_03_l"),
            FName("middle_01_l"), FName("middle_02_l"), FName("middle_03_l"),
            FName("pinky_01_l"),  FName("pinky_02_l"),  FName("pinky_03_l"),
            FName("ring_01_l"),   FName("ring_02_l"),   FName("ring_03_l"),
            FName("thumb_01_l"),  FName("thumb_02_l"),  FName("thumb_03_l"),
            // Right fingers
            FName("index_01_r"),  FName("index_02_r"),  FName("index_03_r"),
            FName("middle_01_r"), FName("middle_02_r"), FName("middle_03_r"),
            FName("pinky_01_r"),  FName("pinky_02_r"),  FName("pinky_03_r"),
            FName("ring_01_r"),   FName("ring_02_r"),   FName("ring_03_r"),
            FName("thumb_01_r"),  FName("thumb_02_r"),  FName("thumb_03_r"),
        };
    }

    /** SMPL-X finger row names (rows where RollOffsetDeg should default to 0.0f). */
    static TSet<FName> MakeFingerRowNames()
    {
        return TSet<FName>{
            FName("leftIndex1"),  FName("leftIndex2"),  FName("leftIndex3"),
            FName("leftMiddle1"), FName("leftMiddle2"), FName("leftMiddle3"),
            FName("leftPinky1"),  FName("leftPinky2"),  FName("leftPinky3"),
            FName("leftRing1"),   FName("leftRing2"),   FName("leftRing3"),
            FName("leftThumb1"),  FName("leftThumb2"),  FName("leftThumb3"),
            FName("rightIndex1"), FName("rightIndex2"), FName("rightIndex3"),
            FName("rightMiddle1"),FName("rightMiddle2"),FName("rightMiddle3"),
            FName("rightPinky1"), FName("rightPinky2"), FName("rightPinky3"),
            FName("rightRing1"),  FName("rightRing2"),  FName("rightRing3"),
            FName("rightThumb1"), FName("rightThumb2"), FName("rightThumb3"),
        };
    }
}

// ---------------------------------------------------------------------------
// Test 1: load DT_GestureSyncBoneRemap_Identity and assert all 52 rows present.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyBoneRemapLoadAll52Rows,
    "Estuary.Body.BoneRemap.LoadAll52Rows",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyBoneRemapLoadAll52Rows::RunTest(const FString& /*Params*/)
{
    UDataTable* Table = EstuaryBodyBoneRemapTestHelpers::LoadIdentityTable();
    if (!TestNotNull(TEXT("DT_GestureSyncBoneRemap_Identity loaded"), Table))
    {
        return false;
    }
    TestEqual(TEXT("DT_GestureSyncBoneRemap_Identity has exactly 52 rows"),
              Table->GetRowMap().Num(), 52);
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 2: every finger row has RollOffsetDeg = 0.0f (identity default).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyBoneRemapDefaultRollIsZero,
    "Estuary.Body.BoneRemap.DefaultRollIsZero",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyBoneRemapDefaultRollIsZero::RunTest(const FString& /*Params*/)
{
    UDataTable* Table = EstuaryBodyBoneRemapTestHelpers::LoadIdentityTable();
    if (!TestNotNull(TEXT("DT_GestureSyncBoneRemap_Identity loaded"), Table))
    {
        return false;
    }

    const TSet<FName> FingerRowNames = EstuaryBodyBoneRemapTestHelpers::MakeFingerRowNames();
    int32 FingerRowsChecked = 0;
    for (const auto& Pair : Table->GetRowMap())
    {
        if (!FingerRowNames.Contains(Pair.Key)) continue;

        const FEstuaryBodyBoneRemapRow* Row =
            reinterpret_cast<const FEstuaryBodyBoneRemapRow*>(Pair.Value);
        if (!TestNotNull(TEXT("Finger row casts to FEstuaryBodyBoneRemapRow"),
                         static_cast<const void*>(Row)))
        {
            return false;
        }
        TestTrue(FString::Printf(TEXT("Finger row '%s' RollOffsetDeg == 0.0f"),
                                 *Pair.Key.ToString()),
                 FMath::IsNearlyEqual(Row->RollOffsetDeg, 0.0f, 1e-6f));
        ++FingerRowsChecked;
    }
    TestEqual(TEXT("All 30 finger rows checked"), FingerRowsChecked, 30);
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 3: ApplyRemap("hips") returns true with UEBoneName=pelvis, RollOffsetDeg=0.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyBoneRemapApplyRemapHits,
    "Estuary.Body.BoneRemap.ApplyRemapHits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyBoneRemapApplyRemapHits::RunTest(const FString& /*Params*/)
{
    UDataTable* Table = EstuaryBodyBoneRemapTestHelpers::LoadIdentityTable();
    if (!TestNotNull(TEXT("DT_GestureSyncBoneRemap_Identity loaded"), Table))
    {
        return false;
    }

    FName UEBoneName;
    float RollOffsetDeg = -999.0f;
    const bool bHit = UEstuaryBodyBoneRemapLibrary::ApplyRemap(
        TEXT("hips"), Table, UEBoneName, RollOffsetDeg);

    TestTrue(TEXT("ApplyRemap('hips') returns true (row found)"), bHit);
    TestEqual(TEXT("ApplyRemap('hips') populates UEBoneName=pelvis"),
              UEBoneName, FName("pelvis"));
    TestTrue(TEXT("ApplyRemap('hips') populates RollOffsetDeg=0.0f"),
             FMath::IsNearlyEqual(RollOffsetDeg, 0.0f, 1e-6f));
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 4: ApplyRemap with unknown bone returns false but populates safe defaults
// (identity FName + 0.0f roll). The warn-once path is exercised; a second call
// for the same name does not log again (covered structurally by the static TSet).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyBoneRemapApplyRemapMisses,
    "Estuary.Body.BoneRemap.ApplyRemapMisses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyBoneRemapApplyRemapMisses::RunTest(const FString& /*Params*/)
{
    UDataTable* Table = EstuaryBodyBoneRemapTestHelpers::LoadIdentityTable();
    if (!TestNotNull(TEXT("DT_GestureSyncBoneRemap_Identity loaded"), Table))
    {
        return false;
    }

    // Use an obviously-unknown bone name so we don't accidentally match a real row.
    const FString UnknownName(TEXT("nonexistent_bone_for_test_only"));

    FName UEBoneName;
    float RollOffsetDeg = -999.0f;
    const bool bHit = UEstuaryBodyBoneRemapLibrary::ApplyRemap(
        UnknownName, Table, UEBoneName, RollOffsetDeg);

    TestFalse(TEXT("ApplyRemap with unknown bone returns false"), bHit);
    TestEqual(TEXT("UEBoneName falls back to FName(*InputName) on miss"),
              UEBoneName, FName(*UnknownName));
    TestTrue(TEXT("RollOffsetDeg falls back to 0.0f on miss"),
             FMath::IsNearlyEqual(RollOffsetDeg, 0.0f, 1e-6f));
    return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Test 5: every UEBoneName in DT_GestureSyncBoneRemap_Identity must be a known
// MetaHuman skeleton bone (Assumption A4 mitigation — guards against silent
// SMPLX_TO_METAHUMAN drift in the codegen script).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyBoneRemapBoneNamesResolveToMetaHuman,
    "Estuary.Body.BoneRemap.BoneNamesResolveToMetaHuman",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyBoneRemapBoneNamesResolveToMetaHuman::RunTest(const FString& /*Params*/)
{
    UDataTable* Table = EstuaryBodyBoneRemapTestHelpers::LoadIdentityTable();
    if (!TestNotNull(TEXT("DT_GestureSyncBoneRemap_Identity loaded"), Table))
    {
        return false;
    }

    const TSet<FName> AllowList = EstuaryBodyBoneRemapTestHelpers::MakeMetaHumanBoneAllowList();
    int32 RowsChecked = 0;
    for (const auto& Pair : Table->GetRowMap())
    {
        const FEstuaryBodyBoneRemapRow* Row =
            reinterpret_cast<const FEstuaryBodyBoneRemapRow*>(Pair.Value);
        if (!TestNotNull(TEXT("Row casts to FEstuaryBodyBoneRemapRow"),
                         static_cast<const void*>(Row)))
        {
            return false;
        }
        TestTrue(FString::Printf(
                     TEXT("Row '%s' UEBoneName '%s' is in MetaHuman allow-list"),
                     *Pair.Key.ToString(), *Row->UEBoneName.ToString()),
                 AllowList.Contains(Row->UEBoneName));
        ++RowsChecked;
    }
    TestEqual(TEXT("All 52 rows checked against MetaHuman allow-list"),
              RowsChecked, 52);
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
