#!/usr/bin/env bash
# ThirdParty/Setup.sh - Fetch livekit/client-sdk-cpp prebuilts for macOS (and Linux on dev hosts).
# Reproduces the 4 logical steps of livekit-examples cmake/LiveKitSDK.cmake
# without requiring a CMake toolchain. Idempotent via .version sentinel.

set -euo pipefail

VERSION="v0.3.4"
VERSION_BARE="${VERSION#v}"

UNAME_S="$(uname -s)"
UNAME_M="$(uname -m)"

case "$UNAME_S" in
    Darwin)
        if [[ "$UNAME_M" == "arm64" ]]; then TRIPLE="macos-arm64"; else TRIPLE="macos-x64"; fi
        EXT="tar.gz"
        ;;
    Linux)
        if [[ "$UNAME_M" == "aarch64" || "$UNAME_M" == "arm64" ]]; then TRIPLE="linux-arm64"; else TRIPLE="linux-x64"; fi
        EXT="tar.gz"
        ;;
    *)
        echo "Setup.sh: unsupported host '$UNAME_S' - run Setup.ps1 on Windows" >&2
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THIRD_PARTY_DIR="$SCRIPT_DIR/livekit-client-sdk-cpp"
VERSION_FILE="$THIRD_PARTY_DIR/.version"
# Platform-specific sentinel — guards against e.g. a Windows .lib layout left behind on the same path.
case "$UNAME_S" in
    Darwin) SENTINEL="$THIRD_PARTY_DIR/lib/liblivekit.dylib" ;;
    Linux)  SENTINEL="$THIRD_PARTY_DIR/lib/liblivekit.so" ;;
esac

if [[ -f "$SENTINEL" && -f "$VERSION_FILE" ]]; then
    existing="$(cat "$VERSION_FILE" | tr -d '[:space:]')"
    if [[ "$existing" == "$VERSION" ]]; then
        echo "Already at $VERSION - skipping"
        exit 0
    else
        echo "Existing version '$existing' != requested '$VERSION' - refreshing"
        rm -rf "$THIRD_PARTY_DIR"
    fi
fi

ASSET="livekit-sdk-${TRIPLE}-${VERSION_BARE}.${EXT}"
URL="https://github.com/livekit/client-sdk-cpp/releases/download/${VERSION}/${ASSET}"

TMP_ROOT="$(mktemp -d -t estuary-livekit.XXXXXXXX)"
trap 'rm -rf "$TMP_ROOT"' EXIT
TMP_TAR="$TMP_ROOT/$ASSET"
TMP_EXTRACT="$TMP_ROOT/extract"
mkdir -p "$TMP_EXTRACT"

echo "Downloading $URL"
curl -fSL --retry 3 --retry-delay 2 -o "$TMP_TAR" "$URL"

echo "Extracting"
tar -xzf "$TMP_TAR" -C "$TMP_EXTRACT"

INNER="$TMP_EXTRACT/livekit-sdk-${TRIPLE}-${VERSION_BARE}"
if [[ ! -d "$INNER" ]]; then
    # Fallback: treat extract root as the SDK root (some releases may flatten on packaging).
    INNER="$TMP_EXTRACT"
fi

rm -rf "$THIRD_PARTY_DIR"
mkdir -p "$THIRD_PARTY_DIR"

for sub in include lib bin; do
    if [[ -d "$INNER/$sub" ]]; then
        mv "$INNER/$sub" "$THIRD_PARTY_DIR/"
    fi
done

printf '%s' "$VERSION" > "$VERSION_FILE"

if [[ ! -f "$SENTINEL" ]]; then
    echo "Setup completed but sentinel '$SENTINEL' missing - prebuilt layout unexpected" >&2
    exit 1
fi

echo "OK - prebuilts at $THIRD_PARTY_DIR"
