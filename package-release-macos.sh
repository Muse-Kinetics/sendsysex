#!/usr/bin/env bash
#
# package-release-macos.sh
#
# Build, sign, notarize, staple, and package a UNIVERSAL (arm64 + x86_64) macOS
# SendSysEx release as a drag-to-Applications .dmg. The macOS counterpart to
# package-release.ps1 (Windows). Produces:
#
#   dist/SendSysEx-vX.Y.Z-macos-universal.dmg
#   dist/SendSysEx-vX.Y.Z-macos-universal.dmg.sha256
#
# DMG shape mirrors KMI's editor releases (see 00_Editors/QuNexus): a single
# "SendSysEx" folder (binary + its data/ + syx/ + README + LICENSE) that the
# user drags onto the Applications alias, over a background with a drag arrow.
#
# Prerequisites (Homebrew): create-dmg
# Xcode command line tools: codesign, xcrun notarytool, xcrun stapler, lipo.
#
# Credentials (NOT stored in this repo):
#   - Signing:      a "Developer ID Application" cert in the login keychain.
#   - Notarizing:   a notarytool keychain profile created once with
#                   `xcrun notarytool store-credentials`. Pass its name via
#                   KMI_NOTARY_PROFILE (see below). The Apple ID / app-specific
#                   password behind it live in the keychain, never in the repo.
#
# Environment overrides:
#   KMI_SIGN_IDENTITY   codesign identity (default: the Kesumo Developer ID,
#                       which is public - it is embedded in every signed binary).
#   KMI_NOTARY_PROFILE  notarytool keychain profile name. REQUIRED unless
#                       KMI_SKIP_NOTARIZE=1.
#   KMI_SKIP_NOTARIZE   set to 1 to build+sign+package but skip notarize/staple
#                       (produces a signed-but-not-notarized dmg for local testing).
#
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_DIR"

SIGN_IDENTITY="${KMI_SIGN_IDENTITY:-Developer ID Application: Kesumo, LLC (J372N6RANB)}"
NOTARY_PROFILE="${KMI_NOTARY_PROFILE:-}"
BACKGROUND="$REPO_DIR/packaging/macos/dmg-background.tiff"

VERSION="$(grep -Eo 'VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | head -1 | awk '{print $2}')"
[ -n "$VERSION" ] || { echo "ERROR: could not read VERSION from CMakeLists.txt"; exit 1; }
VOLUME="SendSysEx"
PKG="SendSysEx-v${VERSION}-macos-universal"
BUILD_DIR="$REPO_DIR/build-universal"
DIST_DIR="$REPO_DIR/dist"

command -v create-dmg >/dev/null || { echo "ERROR: create-dmg not found (brew install create-dmg)"; exit 1; }
[ -f "$BACKGROUND" ] || { echo "ERROR: DMG background not found at $BACKGROUND"; exit 1; }
if [ -z "$NOTARY_PROFILE" ] && [ "${KMI_SKIP_NOTARIZE:-0}" != "1" ]; then
    echo "ERROR: set KMI_NOTARY_PROFILE to your notarytool keychain profile name,"
    echo "       or set KMI_SKIP_NOTARIZE=1 to build a signed-but-not-notarized dmg."
    exit 1
fi

echo "==> SendSysEx v$VERSION - universal macOS release"

# 1) Build universal (arm64 + x86_64) Release from clean config.
echo "==> Configuring + building universal Release ..."
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" >/dev/null
cmake --build "$BUILD_DIR" --config Release -j >/dev/null
lipo -archs "$BUILD_DIR/SendSysEx" | grep -q 'arm64' && lipo -archs "$BUILD_DIR/SendSysEx" | grep -q 'x86_64' \
    || { echo "ERROR: build is not universal"; exit 1; }

# 2) Stage the "SendSysEx" folder outside the repo (Dropbox grabs transient
#    locks on freshly-written files in-repo, which breaks create-dmg's copy).
STAGE="$(mktemp -d)/dmgroot"
FOLDER="$STAGE/$VOLUME"
mkdir -p "$FOLDER"
cp "$BUILD_DIR/SendSysEx" "$FOLDER/"
cp -R "$REPO_DIR/data" "$REPO_DIR/syx" "$FOLDER/"
cp "$REPO_DIR/README.md" "$REPO_DIR/LICENSE" "$FOLDER/"

# 3) Sign the binary: hardened runtime + secure timestamp (both required for
#    notarization).
echo "==> Signing the universal binary ..."
codesign --force --options runtime --timestamp --sign "$SIGN_IDENTITY" "$FOLDER/SendSysEx"
codesign --verify --strict "$FOLDER/SendSysEx"

# 4) Build the drag-to-Applications DMG (window/icon geometry matches the
#    editor releases: folder at 160,220 -> Applications alias at 375,220).
mkdir -p "$DIST_DIR"
OUT="$DIST_DIR/$PKG.dmg"

# Clean up anything a previous FAILED/interrupted run left behind. create-dmg
# stages a read-write "rw.<name>.dmg" and mounts it at /Volumes/<volume>; a run
# that dies mid-build leaves both, and the next run then loops on
# "couldn't unmount ... Resource busy". Detach the stale volume and remove the
# leftover intermediate + output first so a prior failure can't poison this run.
while mount | grep -q " /Volumes/$VOLUME "; do
    dev="$(mount | grep " /Volumes/$VOLUME " | awk '{print $1}' | head -1)"
    echo "==> Detaching stale volume $dev (/Volumes/$VOLUME) ..."
    hdiutil detach "$dev" -force >/dev/null 2>&1 || break
done
rm -f "$OUT" "$DIST_DIR/rw.$PKG.dmg"
echo "==> Building DMG ..."
create-dmg \
    --volname "$VOLUME" \
    --window-pos 200 120 --window-size 530 380 \
    --icon-size 100 \
    --icon "$VOLUME" 160 220 \
    --app-drop-link 375 220 \
    --background "$BACKGROUND" \
    "$OUT" "$STAGE/" \
  || create-dmg --volname "$VOLUME" "$OUT" "$STAGE/"

# 5) Sign the DMG, then notarize + staple (unless skipped).
xattr -cr "$OUT"
codesign --force --options runtime --timestamp --sign "$SIGN_IDENTITY" "$OUT"
if [ "${KMI_SKIP_NOTARIZE:-0}" = "1" ]; then
    echo "==> KMI_SKIP_NOTARIZE=1 - skipping notarize/staple (dmg is signed but NOT notarized)."
else
    echo "==> Notarizing (profile: $NOTARY_PROFILE) ..."
    xcrun notarytool submit "$OUT" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$OUT"
    xcrun stapler validate "$OUT"
    spctl -a -vvv -t open --context context:primary-signature "$OUT" || true
fi

# 6) Checksum next to the artifact.
( cd "$DIST_DIR" && shasum -a 256 "$PKG.dmg" > "$PKG.dmg.sha256" )
rm -rf "$(dirname "$STAGE")"

echo ""
echo "==> Done:"
echo "    $OUT"
echo "    $OUT.sha256"
