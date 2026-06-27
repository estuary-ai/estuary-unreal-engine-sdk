// EstuaryDeveloperSettings.cpp — UEstuaryDeveloperSettings implementation.
//
// Most logic is in the header (inline Get(), GetCategoryName(), GetSectionName()).
// GetEffectiveAuthToken() lives here because it touches FPlatformMisc.
// Pitfall 23: IWYU — include only what is used.

#include "EstuaryDeveloperSettings.h"

#include "HAL/PlatformMisc.h"

FString UEstuaryDeveloperSettings::GetEffectiveAuthToken()
{
    // 1. UEstuarySecretSettings::AuthToken wins when set. It persists to the local,
    //    untracked Config/DefaultEstuarySecret.ini, so it survives editor restarts
    //    WITHOUT being committed to version control (Option A — see EstuaryDeveloperSettings.h).
    if (const UEstuarySecretSettings* Secret = UEstuarySecretSettings::Get())
    {
        if (!Secret->AuthToken.IsEmpty())
        {
            return Secret->AuthToken;
        }
    }

    // 2. Fallback: ESTUARY_API_KEY env var (CI / headless / no-file path). Returns "" if unset.
    return FPlatformMisc::GetEnvironmentVariable(TEXT("ESTUARY_API_KEY"));
}
