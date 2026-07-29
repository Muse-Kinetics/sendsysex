<#
.SYNOPSIS
    Builds and stages a SendSysEx Windows release package for GitHub Releases.

.DESCRIPTION
    Configures and builds SendSysEx (Release, both WinMM and WMS backends),
    stages the executable plus the runtime data/ and syx/ assets it needs at
    runtime, then zips that folder and writes a .sha256 checksum next to it
    under dist/ in the repo.

    Staging and zipping happen in a temp directory OUTSIDE this repo, not in
    dist/ directly: this repo lives in a Dropbox-synced folder, and Dropbox
    grabs transient locks on freshly-written files while it hashes them for
    upload, which reliably breaks Compress-Archive when it immediately tries
    to read dozens of just-copied files. Only the two finished output files
    (the zip and its checksum) are copied into the repo's dist/ folder.

    Code signing is NOT done by this script. Run it once to produce the
    staged, unsigned build; sign the exe at the path printed under "Staged
    exe:" with signtool; then re-run with -SkipBuild to re-zip the signed
    exe without rebuilding (the staging path is stable per version, so the
    same signed file is picked back up).

.PARAMETER Configuration
    CMake build configuration. Default: Release.

.PARAMETER OutputDir
    Directory the final zip + checksum are written under, inside the repo.
    Default: dist.

.PARAMETER SkipBuild
    Skip configure/build and reuse the exe already sitting in the staging
    folder (e.g. after signing it). Still refreshes data/syx/README/LICENSE
    and re-zips.

.PARAMETER CMakeExe
    Path to cmake.exe to use. Default: cmake (whatever is on PATH).

.PARAMETER Generator
    CMake generator to configure with. Default: "Visual Studio 17 2022".
    Override on machines without VS2022 installed, e.g. -Generator
    "Visual Studio 16 2019".

.EXAMPLE
    .\package-release.ps1
    Build + stage + zip an unsigned package.

.EXAMPLE
    .\package-release.ps1 -SkipBuild
    After signing the staged exe in place, re-zip without rebuilding.
#>
param(
    [string]$Configuration = "Release",
    [string]$OutputDir = "dist",
    [switch]$SkipBuild,
    [string]$CMakeExe = "cmake",
    [string]$Generator = "Visual Studio 17 2022"
)

$ErrorActionPreference = "Stop"

$repoRoot = $PSScriptRoot
Set-Location $repoRoot

$buildDir = Join-Path $repoRoot "build"

if (-not $SkipBuild) {
    Write-Host "==> Configuring ($Configuration, $Generator)..." -ForegroundColor Cyan
    & $CMakeExe -G $Generator -A x64 -B $buildDir
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

    Write-Host "==> Building ($Configuration)..." -ForegroundColor Cyan
    & $CMakeExe --build $buildDir --config $Configuration
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed." }
}

$versionHeader = Join-Path $buildDir "generated\version.h"
if (-not (Test-Path $versionHeader)) {
    throw "generated\version.h not found at '$versionHeader' - build first (omit -SkipBuild)."
}
$versionMatch = Select-String -Path $versionHeader -Pattern '#define SENDSYSEX_VERSION "([^"]+)"'
if (-not $versionMatch) { throw "Could not parse SENDSYSEX_VERSION out of $versionHeader" }
$version = $versionMatch.Matches[0].Groups[1].Value
Write-Host "==> Packaging SendSysEx v$version" -ForegroundColor Cyan

$packageName = "SendSysEx-v$version-win-x64"

# Outside Dropbox on purpose - see script header comment.
$stageDir = Join-Path $env:TEMP "sendsysex-release-staging\$packageName"
$exeSource = Join-Path $buildDir "$Configuration\SendSysEx.exe"
$exeStaged = Join-Path $stageDir "SendSysEx.exe"

if (-not $SkipBuild) {
    if (-not (Test-Path $exeSource)) { throw "Built exe not found at '$exeSource'." }
    New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
    Copy-Item -Force $exeSource $exeStaged
} else {
    if (-not (Test-Path $exeStaged)) {
        throw "-SkipBuild given but no staged exe at '$exeStaged' - run without -SkipBuild first."
    }
}

Write-Host "==> Staging runtime assets to $stageDir ..." -ForegroundColor Cyan
foreach ($item in @("data", "syx", "README.md", "LICENSE")) {
    $source = Join-Path $repoRoot $item
    if (-not (Test-Path $source)) { throw "Expected release asset not found: $source" }
    $dest = Join-Path $stageDir $item
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    Copy-Item -Recurse -Force $source $dest
}

$tempZipPath = Join-Path $env:TEMP "sendsysex-release-staging\$packageName.zip"
if (Test-Path $tempZipPath) { Remove-Item -Force $tempZipPath }
Write-Host "==> Zipping (in temp, outside Dropbox)..." -ForegroundColor Cyan
Compress-Archive -Path "$stageDir\*" -DestinationPath $tempZipPath -CompressionLevel Optimal

New-Item -ItemType Directory -Force -Path (Join-Path $repoRoot $OutputDir) | Out-Null
$zipPath = Join-Path $repoRoot "$OutputDir\$packageName.zip"
$hashPath = "$zipPath.sha256"

# Only these two already-finished files touch the Dropbox-synced repo tree;
# retry in case Dropbox still grabs a brief lock on the destination.
$maxAttempts = 6
for ($attempt = 1; $attempt -le $maxAttempts; $attempt++) {
    try {
        Copy-Item -Force $tempZipPath $zipPath -ErrorAction Stop
        break
    } catch {
        if ($attempt -eq $maxAttempts) { throw }
        Write-Host "    Copy attempt $attempt failed (likely Dropbox), retrying in 2s..." -ForegroundColor DarkYellow
        Start-Sleep -Seconds 2
    }
}

$hash = Get-FileHash -Algorithm SHA256 $zipPath
$hashLine = "$($hash.Hash.ToLower())  $packageName.zip"
Set-Content -Path $hashPath -Value $hashLine -Encoding ascii
Write-Host "==> Wrote $zipPath" -ForegroundColor Cyan
Write-Host "==> SHA256: $($hash.Hash)" -ForegroundColor Cyan

$signature = Get-AuthenticodeSignature $exeStaged
if ($signature.Status -ne "Valid") {
    Write-Host ""
    Write-Host "==> UNSIGNED build. Staged exe: $exeStaged" -ForegroundColor Yellow
    Write-Host "    Before publishing to GitHub Releases:" -ForegroundColor Yellow
    Write-Host "      1. signtool sign /fd SHA256 /a /tr http://timestamp.digicert.com /td SHA256 `"$exeStaged`"" -ForegroundColor Yellow
    Write-Host "      2. Re-run: .\package-release.ps1 -SkipBuild   (re-zips the signed exe)" -ForegroundColor Yellow
} else {
    Write-Host ""
    Write-Host "==> Exe is signed (signer: $($signature.SignerCertificate.Subject))." -ForegroundColor Green
    Write-Host "    Ready to attach $zipPath to a GitHub Release." -ForegroundColor Green
}
