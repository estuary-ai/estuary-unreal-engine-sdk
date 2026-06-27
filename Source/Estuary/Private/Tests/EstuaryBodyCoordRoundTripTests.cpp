// EstuaryBodyCoordRoundTripTests.cpp — UE-06 HARD CI GATE.
//
// Loads the vendored Private/Tests/Fixtures/coord_roundtrip_v1.json (committed fixture).
// Asserts < 1e-4 quat error per bone × 52 bones × 6 poses = 312 quat checks
// PLUS < 1e-3 cm hips error per pose × 6 poses = 6 hips checks
// = 318 total checks, all of which must pass before downstream rendering work starts
// (hard CI gate).
//
// On failure, calls EstuaryBodyFrameTransform::DiagnoseQuatResidual to name which
// of the 4 SMPL-X→UE conversion failure modes (AxisSwap / SignFlip / Handedness /
// Roll) the residual matches. This cuts every UE-04/05 iteration debug loop down
// from hours of editor iteration to seconds of math-residual classification.
//
// HOW TO RUN:
//   1. Open the UE 5.6+ project in the editor.
//   2. Window > Session Frontend > Automation tab.
//   3. Filter by "Estuary.Body.CoordRoundTrip".
//   4. Select the test and click "Run Tests".
//
// Pitfall 23: IWYU only.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "EstuaryBodyFrameTransform.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEstuaryBodyCoordRoundTrip6Pose,
    "Estuary.Body.CoordRoundTrip.6Pose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEstuaryBodyCoordRoundTrip6Pose::RunTest(const FString& /*Params*/)
{
    // Resolve fixture path — vendored inside the plugin so this test is self-contained
    // (no external repo dependency). Lives under Private/Tests/Fixtures/.
    const TSharedPtr<IPlugin> EstuaryPlugin = IPluginManager::Get().FindPlugin(TEXT("Estuary"));
    if (!EstuaryPlugin.IsValid())
    {
        AddError(TEXT("UE-06: could not locate the Estuary plugin to resolve the fixture path."));
        return false;
    }
    const FString FixturePath = EstuaryPlugin->GetBaseDir()
        / TEXT("Source/Estuary/Private/Tests/Fixtures/coord_roundtrip_v1.json");

    FString FixtureContent;
    if (!FFileHelper::LoadFileToString(FixtureContent, *FixturePath))
    {
        AddError(FString::Printf(TEXT("UE-06: could not load fixture at %s."), *FixturePath));
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    auto Reader = TJsonReaderFactory<>::Create(FixtureContent);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        AddError(TEXT("UE-06: fixture JSON deserialization failed."));
        return false;
    }

    // Validate fixture version — guards against silent schema drift.
    int32 Version = 0;
    if (!Root->TryGetNumberField(TEXT("version"), Version) || Version != 1)
    {
        AddError(FString::Printf(TEXT("UE-06: fixture version mismatch. Expected 1, got %d."), Version));
        return false;
    }

    constexpr float kQuatEpsilon = 1e-4f;
    constexpr float kHipsEpsilonCm = 1e-3f;
    int32 PassCount = 0;
    int32 FailCount = 0;

    const TArray<TSharedPtr<FJsonValue>>* PosesArr = nullptr;
    if (!Root->TryGetArrayField(TEXT("poses"), PosesArr) || !PosesArr || PosesArr->Num() != 6)
    {
        AddError(FString::Printf(TEXT("UE-06: expected exactly 6 poses; got %d"),
            PosesArr ? PosesArr->Num() : 0));
        return false;
    }

    for (const TSharedPtr<FJsonValue>& PoseVal : *PosesArr)
    {
        const TSharedPtr<FJsonObject> Pose = PoseVal->AsObject();
        if (!Pose.IsValid())
        {
            AddError(TEXT("UE-06: pose entry is not a JSON object"));
            ++FailCount;
            continue;
        }

        const FString PoseName = Pose->GetStringField(TEXT("name"));

        // Per-bone quaternion check (52 bones × 6 poses = 312 total quat checks).
        const TSharedPtr<FJsonObject> SmplxQuats = Pose->GetObjectField(TEXT("smplx_quats"));
        const TSharedPtr<FJsonObject> ExpectedQuats = Pose->GetObjectField(TEXT("expected_ue_quats"));

        if (!SmplxQuats.IsValid() || !ExpectedQuats.IsValid())
        {
            AddError(FString::Printf(TEXT("UE-06 FAIL: pose '%s' missing smplx_quats or expected_ue_quats"),
                *PoseName));
            ++FailCount;
            continue;
        }

        for (const auto& BonePair : SmplxQuats->Values)
        {
            const FString& BoneName = BonePair.Key;
            const TArray<TSharedPtr<FJsonValue>>& InArr = BonePair.Value->AsArray();
            if (InArr.Num() != 4)
            {
                AddError(FString::Printf(TEXT("UE-06 FAIL: pose '%s' bone '%s' smplx_quat has %d components; expected 4"),
                    *PoseName, *BoneName, InArr.Num()));
                ++FailCount;
                continue;
            }
            const FQuat InQuat(
                InArr[0]->AsNumber(), InArr[1]->AsNumber(),
                InArr[2]->AsNumber(), InArr[3]->AsNumber());

            const FQuat OutQuat = EstuaryBodyFrameTransform::ToUEQuatFromGestureSync(InQuat);

            const TArray<TSharedPtr<FJsonValue>>& ExpArr =
                ExpectedQuats->GetArrayField(BoneName);
            if (ExpArr.Num() != 4)
            {
                AddError(FString::Printf(TEXT("UE-06 FAIL: pose '%s' bone '%s' expected_ue_quat has %d components; expected 4"),
                    *PoseName, *BoneName, ExpArr.Num()));
                ++FailCount;
                continue;
            }
            const FQuat ExpectedQuat(
                ExpArr[0]->AsNumber(), ExpArr[1]->AsNumber(),
                ExpArr[2]->AsNumber(), ExpArr[3]->AsNumber());

            const float QuatErr = FQuat::ErrorAutoNormalize(OutQuat, ExpectedQuat);
            if (QuatErr > kQuatEpsilon)
            {
                ++FailCount;
                // 4-mode diagnostic: name which conversion-variant residual
                // the observed-vs-expected mismatch matches. This cuts hours of
                // PIE iteration to seconds of math-residual classification.
                const auto Mode = EstuaryBodyFrameTransform::DiagnoseQuatResidual(
                    ExpectedQuat, OutQuat, kQuatEpsilon);
                AddError(FString::Printf(
                    TEXT("UE-06 FAIL: pose '%s' bone '%s' quat err %.6f > %.6f. Diagnosed mode: %s. "
                         "Expected (%.4f,%.4f,%.4f,%.4f); got (%.4f,%.4f,%.4f,%.4f)."),
                    *PoseName, *BoneName, QuatErr, kQuatEpsilon,
                    EstuaryBodyFrameTransform::EConversionFailureModeToString(Mode),
                    ExpectedQuat.X, ExpectedQuat.Y, ExpectedQuat.Z, ExpectedQuat.W,
                    OutQuat.X, OutQuat.Y, OutQuat.Z, OutQuat.W));
            }
            else
            {
                ++PassCount;
            }
        }

        // Per-pose hips translation check (1 hips × 6 poses = 6 total hips checks).
        const TArray<TSharedPtr<FJsonValue>>& InHipsArr =
            Pose->GetArrayField(TEXT("smplx_hips_world"));
        if (InHipsArr.Num() != 3)
        {
            AddError(FString::Printf(TEXT("UE-06 FAIL: pose '%s' smplx_hips_world has %d components; expected 3"),
                *PoseName, InHipsArr.Num()));
            ++FailCount;
            continue;
        }
        const FVector InHipsMeters(
            InHipsArr[0]->AsNumber(), InHipsArr[1]->AsNumber(), InHipsArr[2]->AsNumber());

        const FVector OutHipsCm = EstuaryBodyFrameTransform::ToUEHipsFromGestureSync(InHipsMeters);

        const TArray<TSharedPtr<FJsonValue>>& ExpHipsArr =
            Pose->GetArrayField(TEXT("expected_ue_hips_cm"));
        if (ExpHipsArr.Num() != 3)
        {
            AddError(FString::Printf(TEXT("UE-06 FAIL: pose '%s' expected_ue_hips_cm has %d components; expected 3"),
                *PoseName, ExpHipsArr.Num()));
            ++FailCount;
            continue;
        }
        const FVector ExpectedHipsCm(
            ExpHipsArr[0]->AsNumber(), ExpHipsArr[1]->AsNumber(), ExpHipsArr[2]->AsNumber());

        if (!OutHipsCm.Equals(ExpectedHipsCm, kHipsEpsilonCm))
        {
            ++FailCount;
            AddError(FString::Printf(
                TEXT("UE-06 FAIL: pose '%s' hips_cm (%.4f,%.4f,%.4f) != expected (%.4f,%.4f,%.4f); residual %.4f cm > %.4f cm"),
                *PoseName,
                OutHipsCm.X, OutHipsCm.Y, OutHipsCm.Z,
                ExpectedHipsCm.X, ExpectedHipsCm.Y, ExpectedHipsCm.Z,
                FVector::Distance(OutHipsCm, ExpectedHipsCm),
                kHipsEpsilonCm));
        }
        else
        {
            ++PassCount;
        }
    }

    AddInfo(FString::Printf(TEXT("UE-06: %d pass / %d fail (target: 312 quat + 6 hips = 318 checks all pass)"),
        PassCount, FailCount));
    TestTrue(FString::Printf(TEXT("UE-06 6-pose math gate: all %d checks pass"), PassCount + FailCount),
        FailCount == 0);

    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
