# Releasing SendSysEx

Windows release packages are built and staged with `package-release.ps1`
(PowerShell, run from a Developer PowerShell for VS 2022 or any shell with
`cmake` and MSBuild/VS2022 on PATH). It does **not** sign the executable -
that's a separate manual step below.

## 1. Bump the version

Edit `project(SendSysEx VERSION X.Y.Z ...)` in `CMakeLists.txt`. That's the
single source of truth - it flows through to the generated `version.h`, the
`SendSysEx vX.Y.Z` startup banner, `--help`, and the packaged zip's filename.

Update the `Version` / `Last updated` line in `README.md` to match.

## 2. Build and stage

```powershell
.\package-release.ps1
```

This configures + builds Release (both WinMM and WMS backends compiled in),
stages `SendSysEx.exe` alongside the `data/`, `syx/`, `README.md`, and
`LICENSE` it needs at runtime, zips the result to
`dist/SendSysEx-vX.Y.Z-win-x64.zip`, and writes a `.sha256` checksum next to
it. It prints the exact staged exe path and a `signtool` command to run next.

Staging happens in a temp directory outside this repo, not in `dist/`
directly - this repo lives in a Dropbox-synced folder, and Dropbox grabs
transient locks on freshly-written files while it hashes them for upload,
which reliably breaks zipping a few dozen just-copied files in place. Only
the finished zip + checksum land in `dist/`.

Verify the unsigned build actually runs before signing:

```powershell
cd dist
Expand-Archive SendSysEx-vX.Y.Z-win-x64.zip -DestinationPath smoke-test
.\smoke-test\SendSysEx.exe -l
Remove-Item -Recurse -Force smoke-test
```

## 3. Sign

Requires the code-signing certificate (SafeNet token / cert store entry at
the office - not available for remote sessions). Sign the **staged** exe
printed by step 2, not the one in `build\Release\`:

```powershell
signtool sign /fd SHA256 /a /tr http://timestamp.digicert.com /td SHA256 "<staged-exe-path>"
```

Verify:

```powershell
Get-AuthenticodeSignature "<staged-exe-path>"
```

## 4. Re-zip the signed build

```powershell
.\package-release.ps1 -SkipBuild
```

Reuses the exe already sitting in the staging folder (now signed), refreshes
`data`/`syx`/`README.md`/`LICENSE`, re-zips, and rewrites the checksum. The
script confirms the signer identity in its output when it detects a valid
signature.

## 5. Tag and publish

```powershell
git tag -a vX.Y.Z -m "SendSysEx vX.Y.Z"
git push origin vX.Y.Z

gh release create vX.Y.Z `
    dist\SendSysEx-vX.Y.Z-win-x64.zip `
    dist\SendSysEx-vX.Y.Z-win-x64.zip.sha256 `
    --title "SendSysEx vX.Y.Z" `
    --notes "..."
```

Write release notes from the commits since the last tag
(`git log <prev-tag>..HEAD --oneline`) - call out anything hardware-relevant
(new firmware payloads, backend changes, breaking CLI flag changes).

## macOS

Not yet covered by `package-release.ps1` (Windows-only script; the exe/zip
layout, signing, and notarization steps all differ). Build per the README's
"Building with CMake" section and package manually if a macOS release is
needed.
