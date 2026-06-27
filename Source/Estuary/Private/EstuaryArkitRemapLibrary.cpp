// EstuaryArkitRemapLibrary.cpp — UEstuaryArkitRemapLibrary implementation.
// Pitfall 6:  Missing-row warning emitted once per unique curve name (WarnedCurves TSet).
// Pitfall 23: IWYU, no CoreMinimal.h.

#include "EstuaryArkitRemap.h"
#include "EstuaryLog.h"
#include "Engine/DataTable.h"
#include "Math/UnrealMathUtility.h"

bool UEstuaryArkitRemapLibrary::ApplyRemap(
    const FString& InputName,
    const UDataTable* Table,
    FString& OutputName,
    float& Scale)
{
    // Identity fallback — caller can use these values safely even on miss.
    OutputName = InputName;
    Scale      = 1.0f;

    if (!Table) return false;

    const FEstuaryArkitRemapRow* Row =
        Table->FindRow<FEstuaryArkitRemapRow>(FName(*InputName),
            TEXT("UEstuaryArkitRemapLibrary::ApplyRemap"),
            /*bWarnIfRowMissing=*/false);  // suppress DataTable's own warning; we handle it

    if (!Row)
    {
        // One-time warning per unknown curve name to avoid log spam (Pitfall 6).
        // WarnedCurves is a process-lifetime static — reset on restart is correct.
        static TSet<FString> WarnedCurves;
        bool bAlreadyInSet = false;
        WarnedCurves.Add(InputName, &bAlreadyInSet);
        if (!bAlreadyInSet)
        {
            UE_LOG(LogEstuary, Warning,
                TEXT("[ArkitRemap] Curve '%s' not found in remap table. "
                     "Using identity mapping. Check your DT_ArkitRemap DataTable "
                     "or set FaceRigProfile=Identity for MetaHuman_ARKit_Ready rigs."),
                *InputName);
        }
        return false;
    }

    OutputName = Row->TargetCurveName.IsEmpty() ? InputName : Row->TargetCurveName;
    Scale      = Row->ScaleFactor;
    return true;
}

void UEstuaryArkitRemapLibrary::ApplyRemapToWeights(
    const UDataTable* Table, TMap<FString, float>& Weights)
{
    if (!Table || Weights.Num() == 0) return;

    TMap<FString, float> Remapped;
    Remapped.Reserve(Weights.Num());

    for (const auto& Pair : Weights)
    {
        FString OutName;
        float   Scale;
        ApplyRemap(Pair.Key, Table, OutName, Scale);

        // Merge strategy: if OutName already exists (two ARKit keys map to same rig curve),
        // use the larger value (max-blend avoids inadvertent cancellation).
        float& Existing = Remapped.FindOrAdd(OutName, 0.0f);
        Existing = FMath::Max(Existing, Pair.Value * Scale);
    }

    Weights = MoveTemp(Remapped);
}
