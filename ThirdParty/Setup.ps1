#Requires -Version 5.1
# ThirdParty/Setup.ps1 — Fetch livekit/client-sdk-cpp prebuilts for Win64.
# Reproduces the 4 logical steps of livekit-examples cmake/LiveKitSDK.cmake
# without requiring a CMake toolchain. Idempotent via .version sentinel.

$ErrorActionPreference = "Stop"

$VERSION   = "v0.3.4"
$VERSION_BARE = $VERSION.TrimStart('v')
$TRIPLE    = "windows-x64"

$ThirdPartyDir = Join-Path $PSScriptRoot "livekit-client-sdk-cpp"
$VersionFile   = Join-Path $ThirdPartyDir ".version"
# Platform-specific sentinel — guards against e.g. a Linux .so layout left behind on the same path.
$Sentinel      = Join-Path $ThirdPartyDir "lib\livekit.lib"

if ((Test-Path $Sentinel) -and (Test-Path $VersionFile))
{
    $existing = (Get-Content $VersionFile -Raw).Trim()
    if ($existing -eq $VERSION)
    {
        Write-Host "Already at $VERSION - skipping"
        exit 0
    }
    else
    {
        Write-Host "Existing version '$existing' != requested '$VERSION' - refreshing"
        Remove-Item -Recurse -Force $ThirdPartyDir
    }
}

$AssetName = "livekit-sdk-$TRIPLE-$VERSION_BARE.zip"
$Url       = "https://github.com/livekit/client-sdk-cpp/releases/download/$VERSION/$AssetName"

$TempRoot   = Join-Path ([System.IO.Path]::GetTempPath()) "estuary-livekit-$([System.Guid]::NewGuid().ToString('N'))"
$TempZip    = Join-Path $TempRoot $AssetName
$TempExtract = Join-Path $TempRoot "extract"

New-Item -ItemType Directory -Path $TempRoot -Force | Out-Null
New-Item -ItemType Directory -Path $TempExtract -Force | Out-Null

try
{
    Write-Host "Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $TempZip -UseBasicParsing

    Write-Host "Extracting"
    Expand-Archive -Path $TempZip -DestinationPath $TempExtract -Force

    $InnerDir = Join-Path $TempExtract "livekit-sdk-$TRIPLE-$VERSION_BARE"
    if (-not (Test-Path $InnerDir))
    {
        # Some releases may not have the wrapping directory; fall back to extract root.
        $InnerDir = $TempExtract
    }

    if (Test-Path $ThirdPartyDir) { Remove-Item -Recurse -Force $ThirdPartyDir }
    New-Item -ItemType Directory -Path $ThirdPartyDir -Force | Out-Null

    foreach ($sub in @("include", "lib", "bin"))
    {
        $src = Join-Path $InnerDir $sub
        if (Test-Path $src)
        {
            Move-Item -Path $src -Destination $ThirdPartyDir -Force
        }
    }

    Set-Content -Path $VersionFile -Value $VERSION -NoNewline

    if (-not (Test-Path $Sentinel))
    {
        throw "Setup completed but sentinel '$Sentinel' missing - prebuilt layout unexpected"
    }

    Write-Host "OK - prebuilts at $ThirdPartyDir"
    exit 0
}
finally
{
    if (Test-Path $TempRoot) { Remove-Item -Recurse -Force $TempRoot -ErrorAction SilentlyContinue }
}
