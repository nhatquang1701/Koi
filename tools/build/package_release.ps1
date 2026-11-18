[CmdletBinding()]
param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../../build/release'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../artifacts/packages'),
    # Optional gated NNUE payload: a network with at least classical-equal
    # strength at equal time. Omit it for packages without a bundled network.
    [string]$NnuePath = '',
    [string]$NnueMetadataPath = '',
    # Optional strength-gate report proving the bundled network was accepted
    # (net-vs-classical match/SPRT). Release packages must pass it; packaging a
    # network without evidence stays available only for tests/local inspection.
    [string]$NnueGateReport = '',
    # Bundled Stockfish-verified CC0 opening book copied flat beside the
    # executables as book.bin for automatic discovery. Pass an empty string only
    # when explicitly omitting the book in isolated tests.
    [string]$BookPath = (Join-Path (Join-Path (Join-Path $PSScriptRoot '../..') 'assets') 'book/koi.bin'),
    # Release-candidate packaging: bundles of the public release must carry a
    # gated network and its gate report. Tests/local inspection may omit both.
    [switch]$ReleasePackage
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildRoot = [System.IO.Path]::GetFullPath($BuildDirectory)
$outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
$isWindowsHost = $env:OS -eq 'Windows_NT'
$isMacOSHost = -not $isWindowsHost -and
    [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::OSX)
$binarySuffix = if ($isWindowsHost) { '.exe' } else { '' }
$platformTag = if ($isWindowsHost) { '' } elseif ($isMacOSHost) { '-macos-arm64' } else { '-linux-x86_64' }
$packageName = "koi-engine-v1.1.6$platformTag"
$packageDirectory = Join-Path $outputRoot $packageName
$archivePath = if ($isWindowsHost) {
    Join-Path $outputRoot "$packageName.zip"
} else {
    Join-Path $outputRoot "$packageName.tar.gz"
}

function Get-Sha256Hex {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.IO.File]::ReadAllBytes($Path)
        return ([System.BitConverter]::ToString($sha256.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha256.Dispose()
    }
}

if (-not (Test-Path -LiteralPath $buildRoot -PathType Container)) {
    throw "Build directory does not exist: $buildRoot"
}
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

# The x86-64 AVX2/AVX-512 sibling builds are only produced on Windows and
# Linux; the macOS arm64 package ships the single native build.
# Release archives ship the engine binaries (and their launcher variants) only.
# Developer tools and the test suite are built from source by developers and are
# deliberately excluded from release packages.
$requiredExecutables = @("koi-engine$binarySuffix")
if (-not $isMacOSHost) {
    $requiredExecutables = @("koi-engine$binarySuffix", "koi-engine-avx2$binarySuffix",
                             "koi-engine-avx512$binarySuffix")
}
function Test-RequiredExecutables {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory
    )

    foreach ($name in $requiredExecutables) {
        if (-not (Test-Path -LiteralPath (Join-Path $Directory $name) -PathType Leaf)) {
            return $false
        }
    }
    return $true
}

$binaryDirectory = $buildRoot
if (-not (Test-RequiredExecutables -Directory $binaryDirectory)) {
    $multiConfigReleaseDirectory = Join-Path $buildRoot 'Release'
    if (-not (Test-RequiredExecutables -Directory $multiConfigReleaseDirectory)) {
        throw "Release executables are missing from '$buildRoot' or '$multiConfigReleaseDirectory'."
    }
    $binaryDirectory = [System.IO.Path]::GetFullPath($multiConfigReleaseDirectory)
}
$licensePath = Join-Path $repositoryRoot 'LICENSE'
if (-not (Test-Path -LiteralPath $licensePath -PathType Leaf)) {
    throw "LICENSE is missing: $licensePath"
}

# Only the exact generated package directory is replaced.  The resolved target
# is checked to stay below the explicitly selected output directory.
$outputPrefix = $outputRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
$resolvedPackage = [System.IO.Path]::GetFullPath($packageDirectory)
if (-not $resolvedPackage.StartsWith($outputPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace package outside the output directory: $resolvedPackage"
}
if (Test-Path -LiteralPath $packageDirectory) {
    Remove-Item -LiteralPath $packageDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $packageDirectory -Force | Out-Null

$manifestPath = Join-Path $outputRoot "$packageName.package.json"

foreach ($name in $requiredExecutables) {
    Copy-Item -LiteralPath (Join-Path $binaryDirectory $name) -Destination (Join-Path $packageDirectory $name)
}
Copy-Item -LiteralPath $licensePath -Destination (Join-Path $packageDirectory 'LICENSE')

$nnueIncluded = $false
$nnueMetadataSha256 = $null
$nnueGate = $null
if (-not [string]::IsNullOrWhiteSpace($NnuePath)) {
    $resolvedNnue = [System.IO.Path]::GetFullPath($NnuePath)
    if (-not (Test-Path -LiteralPath $resolvedNnue -PathType Leaf)) {
        throw "NNUE network is missing: $resolvedNnue"
    }
    # Place koi.nnue directly beside the executables so the user sees a clean
    # directory with only the binaries, neural network, and root LICENSE.
    $nnueTarget = Join-Path $packageDirectory 'koi.nnue'
    Copy-Item -LiteralPath $resolvedNnue -Destination $nnueTarget
    $nnueSha = Get-Sha256Hex $nnueTarget
    if (-not [string]::IsNullOrWhiteSpace($NnueMetadataPath)) {
        $resolvedMetadata = [System.IO.Path]::GetFullPath($NnueMetadataPath)
        if (-not (Test-Path -LiteralPath $resolvedMetadata -PathType Leaf)) {
            throw "NNUE metadata is missing: $resolvedMetadata"
        }
        $nnueMetadataSha256 = Get-Sha256Hex $resolvedMetadata
    }
    $nnueIncluded = $true

    if (-not [string]::IsNullOrWhiteSpace($NnueGateReport)) {
        $resolvedGate = [System.IO.Path]::GetFullPath($NnueGateReport)
        if (-not (Test-Path -LiteralPath $resolvedGate -PathType Leaf)) {
            throw "NNUE gate report is missing: $resolvedGate"
        }
        try {
            $gate = Get-Content -LiteralPath $resolvedGate -Raw | ConvertFrom-Json
        } catch {
            throw "NNUE gate report is not valid JSON: $resolvedGate"
        }
        $gateDecision = [string]$gate.decision
        if ($gateDecision -notin @('accept', 'accepted', 'promote', 'promoted', 'candidate-stronger')) {
            throw "NNUE gate report decision '$gateDecision' does not approve promotion."
        }
        $gateSha = @($gate.candidate_sha256, $gate.network_sha256, $gate.nnue_sha256,
                     $gate.candidate_sha) |
            Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) } |
            Select-Object -First 1
        if ([string]::IsNullOrWhiteSpace([string]$gateSha)) {
            throw 'NNUE gate report does not record the accepted network SHA-256.'
        }
        if (([string]$gateSha).ToLowerInvariant() -cne $nnueSha.ToLowerInvariant()) {
            throw 'NNUE gate report SHA-256 does not match the bundled network.'
        }
        $nnueGate = [ordered]@{
            report_sha256 = Get-Sha256Hex $resolvedGate
            decision = $gateDecision
        }
    }
}

$bookIncluded = $false
$bookSha = $null
if (-not [string]::IsNullOrWhiteSpace($BookPath)) {
    $resolvedBook = [System.IO.Path]::GetFullPath($BookPath)
    if (-not (Test-Path -LiteralPath $resolvedBook -PathType Leaf)) {
        throw "Opening book is missing: $resolvedBook"
    }
    $bookTarget = Join-Path $packageDirectory 'book.bin'
    Copy-Item -LiteralPath $resolvedBook -Destination $bookTarget
    $bookSha = Get-Sha256Hex $bookTarget
    $bookIncluded = $true
}

if ($ReleasePackage) {
    if (-not $nnueIncluded) {
        throw 'A release package must bundle a gated NNUE network (-NnuePath).'
    }
    if ($null -eq $nnueGate) {
        throw 'A release package must pass -NnueGateReport with an accepted gate decision.'
    }
    if (-not $bookIncluded) {
        throw 'A release package must bundle the CC0 opening book (-BookPath).'
    }
}

# Keep provenance and checksums out of the user-facing archive so extracting
# a release leaves only the executables, koi.nnue, book.bin, and LICENSE.

$sourceCommit = $env:GITHUB_SHA
if ($sourceCommit -notmatch '^[0-9a-f]{40,64}$') {
    $sourceCommitOutput = @(& git -C $repositoryRoot rev-parse HEAD 2>$null)
    $gitExitCode = $LASTEXITCODE
    $sourceCommit = if ($sourceCommitOutput.Count -gt 0) { $sourceCommitOutput[0].Trim() } else { 'unknown' }
    if ($gitExitCode -ne 0 -or $sourceCommit -notmatch '^[0-9a-f]{40,64}$') {
        $sourceCommit = 'unknown'
    }
}
if ($sourceCommit -notmatch '^[0-9a-f]{40,64}$') {
    throw 'Release packaging requires a source commit; set GITHUB_SHA or run from a Git checkout.'
}
# Disable autocrlf for the dirty probe: a CRLF worktree must not be reported as
# dirty when the index stores LF (the 1.1.4 Docker packaging hit exactly this).
$statusOutput = @(& git -c core.autocrlf=false -C $repositoryRoot status --porcelain --untracked-files=all 2>$null)
$gitStatusExitCode = $LASTEXITCODE
if ($gitStatusExitCode -ne 0) {
    throw 'Unable to determine whether the source checkout is dirty.'
}
$sourceDirty = $statusOutput.Count -gt 0
if ($sourceDirty) {
    Write-Output "source_dirty=true first_offender=$($statusOutput[0])"
}
$packageFiles = @(Get-ChildItem -LiteralPath $packageDirectory -File -Recurse |
    Sort-Object { $_.FullName.Substring($packageDirectory.Length + 1).Replace('\', '/') } |
    ForEach-Object {
        $relativePath = $_.FullName.Substring($packageDirectory.Length + 1).Replace('\', '/')
        [ordered]@{
            path = $relativePath
            size_bytes = $_.Length
            sha256 = Get-Sha256Hex $_.FullName
        }
    })

if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
}
if ($isWindowsHost) {
    Compress-Archive -Path (Join-Path $packageDirectory '*') -DestinationPath $archivePath -CompressionLevel Optimal
} else {
    & tar -czf $archivePath -C $packageDirectory .
    if ($LASTEXITCODE -ne 0) {
        throw "tar failed with exit code $LASTEXITCODE."
    }
}
$archiveHash = Get-Sha256Hex -Path $archivePath
Set-Content -LiteralPath "$archivePath.sha256" -Value "$archiveHash  $([System.IO.Path]::GetFileName($archivePath))" -Encoding ASCII

$manifest = [ordered]@{
    schema = 'koi-engine-package-v1'
    version = '1.1.6'
    platform = if ($isWindowsHost) { 'windows-x86_64' } elseif ($isMacOSHost) { 'macos-arm64' } else { 'linux-x86_64' }
    build_configuration = 'Release'
    archive = [ordered]@{
        name = [System.IO.Path]::GetFileName($archivePath)
        size_bytes = (Get-Item -LiteralPath $archivePath).Length
        sha256 = $archiveHash
    }
    files = $packageFiles
    book_included = [bool]$bookIncluded
    book_sha256 = $bookSha
    tablebases_included = $false
    nnue_included = [bool]$nnueIncluded
    nnue_metadata_sha256 = $nnueMetadataSha256
    nnue_gate = $nnueGate
    provenance = [ordered]@{
        source_commit = $sourceCommit
        source_dirty = [bool]$sourceDirty
        source_ref = if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_REF)) { $env:GITHUB_REF } else { $null }
        created_utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Output "package_directory=$packageDirectory"
Write-Output "manifest=$manifestPath"
Write-Output "archive=$archivePath"
Write-Output "archive_sha256=$archiveHash"
Write-Output "nnue_included=$($nnueIncluded.ToString().ToLowerInvariant())"
Write-Output "nnue_gate_decision=$(if ($null -ne $nnueGate) { $nnueGate.decision } else { 'none' })"
Write-Output "release_package=$($ReleasePackage.ToString().ToLowerInvariant())"
