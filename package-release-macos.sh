#!/usr/bin/env bash
#
# package-release-macos.sh
#
# Build, sign, notarize, and staple a UNIVERSAL (arm64 + x86_64) macOS SendSysEx
# release as a notarized .pkg installer. The macOS counterpart to
# package-release.ps1 (Windows). Produces:
#
#   dist/SendSysEx-vX.Y.Z-macos-universal.pkg
#   dist/SendSysEx-vX.Y.Z-macos-universal.pkg.sha256
#
# A .pkg (not a .dmg) because it is the only clean path for a command-line tool:
# a loose CLI binary can't carry a stapled notarization ticket, so a drag-install
# from a .dmg leaves the copied binary quarantined and Gatekeeper blocks it
# ("developer cannot be verified"). The .pkg installs to /Applications/SendSysEx
# without the quarantine flag, symlinks it into /usr/local/bin (PATH), and is
# itself stapled - so it double-clicks and installs with no Gatekeeper prompt and
# no `xattr` step.
#
# Prerequisites: Xcode command line tools (codesign, pkgbuild, xcrun notarytool,
# xcrun stapler, lipo).
#
# Credentials (NOT stored in this repo; see ~/Desktop/refresh_keys.sh):
#   - App signing:       a "Developer ID Application" cert in the login keychain.
#   - Installer signing:  a "Developer ID Installer" cert in the login keychain.
#   - Notarizing:        a notarytool keychain profile (its name only).
# Environment overrides (refresh_keys.sh sets DEVELOPER_ID / DEVELOPER_ID_INSTALLER
# / APPLE_KEYCHAIN_PROFILE, which are picked up here):
#   KMI_SIGN_IDENTITY / DEVELOPER_ID             app codesign identity
#   KMI_INSTALLER_IDENTITY / DEVELOPER_ID_INSTALLER  pkg signing identity
#   KMI_NOTARY_PROFILE / APPLE_KEYCHAIN_PROFILE  notarytool keychain profile name
#   KMI_SKIP_NOTARIZE=1  build+sign but skip notarize/staple (local testing).
#
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_DIR"

SIGN_IDENTITY="${KMI_SIGN_IDENTITY:-${DEVELOPER_ID:-Developer ID Application: Kesumo, LLC (J372N6RANB)}}"
INSTALLER_IDENTITY="${KMI_INSTALLER_IDENTITY:-${DEVELOPER_ID_INSTALLER:-Developer ID Installer: Kesumo, LLC (J372N6RANB)}}"
NOTARY_PROFILE="${KMI_NOTARY_PROFILE:-${APPLE_KEYCHAIN_PROFILE:-}}"

VERSION="$(grep -Eo 'VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | head -1 | awk '{print $2}')"
[ -n "$VERSION" ] || { echo "ERROR: could not read VERSION from CMakeLists.txt"; exit 1; }
PKG_NAME="SendSysEx-v${VERSION}-macos-universal"
IDENTIFIER="com.kesumo.sendsysex"
INSTALL_DIR="/Applications/SendSysEx"
BUILD_DIR="$REPO_DIR/build-universal"
DIST_DIR="$REPO_DIR/dist"

if [ -z "$NOTARY_PROFILE" ] && [ "${KMI_SKIP_NOTARIZE:-0}" != "1" ]; then
    echo "ERROR: set KMI_NOTARY_PROFILE (or APPLE_KEYCHAIN_PROFILE via refresh_keys.sh) to your"
    echo "       notarytool keychain profile, or set KMI_SKIP_NOTARIZE=1 to skip notarization."
    exit 1
fi

echo "==> SendSysEx v$VERSION - universal macOS .pkg"

# 1) Build universal (arm64 + x86_64) Release from clean config.
echo "==> Configuring + building universal Release ..."
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" >/dev/null
cmake --build "$BUILD_DIR" --config Release -j >/dev/null
lipo -archs "$BUILD_DIR/SendSysEx" | grep -q 'arm64' && lipo -archs "$BUILD_DIR/SendSysEx" | grep -q 'x86_64' \
    || { echo "ERROR: build is not universal"; exit 1; }

# 2) Stage the payload (what installs at INSTALL_DIR): binary + its runtime
#    data/ + syx/ + README + LICENSE. Staged outside the repo (Dropbox grabs
#    transient locks on freshly-written files in-repo).
STAGE="$(mktemp -d)"
PAYLOAD="$STAGE/payload"
SCRIPTS="$STAGE/scripts"
mkdir -p "$PAYLOAD" "$SCRIPTS"
cp "$BUILD_DIR/SendSysEx" "$PAYLOAD/"
cp -R "$REPO_DIR/data" "$REPO_DIR/syx" "$PAYLOAD/"
cp "$REPO_DIR/README.md" "$REPO_DIR/LICENSE" "$PAYLOAD/"
# Strip extended attributes so pkgbuild doesn't embed ._AppleDouble files.
xattr -cr "$PAYLOAD"

# 3) Sign the binary: hardened runtime + secure timestamp (required for notarization).
echo "==> Signing the universal binary ..."
codesign --force --options runtime --timestamp --sign "$SIGN_IDENTITY" "$PAYLOAD/SendSysEx"
codesign --verify --strict "$PAYLOAD/SendSysEx"

# 4) postinstall: symlink into PATH, then reveal the install folder in the
#    logged-in user's Finder. postinstall runs as root, so hop into the console
#    user's GUI session (launchctl asuser + sudo -u) for the window to appear
#    for them, not root. Failures here never fail the install.
cat > "$SCRIPTS/postinstall" <<'POSTINSTALL'
#!/bin/bash
mkdir -p /usr/local/bin
ln -sf /Applications/SendSysEx/SendSysEx /usr/local/bin/SendSysEx

user="$(stat -f %Su /dev/console 2>/dev/null || true)"
uid="$(id -u "$user" 2>/dev/null || true)"
if [ -n "$uid" ] && [ "$user" != "root" ] && [ "$user" != "loginwindow" ]; then
    launchctl asuser "$uid" sudo -u "$user" open /Applications/SendSysEx 2>/dev/null || true
fi
exit 0
POSTINSTALL
chmod +x "$SCRIPTS/postinstall"

# 5) Build + sign the component .pkg (installs PAYLOAD's contents to INSTALL_DIR).
mkdir -p "$DIST_DIR"
OUT="$DIST_DIR/$PKG_NAME.pkg"
rm -f "$OUT"
echo "==> Building + signing .pkg ..."
pkgbuild \
    --root "$PAYLOAD" \
    --scripts "$SCRIPTS" \
    --identifier "$IDENTIFIER" \
    --version "$VERSION" \
    --install-location "$INSTALL_DIR" \
    --sign "$INSTALLER_IDENTITY" \
    "$OUT"
pkgutil --check-signature "$OUT" | grep -i 'Status:' || true

# 6) Notarize + staple (unless skipped).
if [ "${KMI_SKIP_NOTARIZE:-0}" = "1" ]; then
    echo "==> KMI_SKIP_NOTARIZE=1 - skipping notarize/staple (pkg is signed but NOT notarized)."
else
    echo "==> Notarizing (profile: $NOTARY_PROFILE) ..."
    xcrun notarytool submit "$OUT" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$OUT"
    xcrun stapler validate "$OUT"
    spctl -a -vvv -t install "$OUT" || true
fi

# 7) Checksum next to the artifact.
( cd "$DIST_DIR" && shasum -a 256 "$PKG_NAME.pkg" > "$PKG_NAME.pkg.sha256" )
rm -rf "$STAGE"

echo ""
echo "==> Done:"
echo "    $OUT"
echo "    $OUT.sha256"
