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
    # Release-candidate packaging: bundles of the public release must carry a
    # gated network and its gate report. Tests/local inspection may omit both.
    [switch]$ReleasePackage,
    # Optional Polyglot opening book, generated with tools/book/build_book.py from
    # the CC0 lichess-org/chess-openings data. The engine auto-detects a book
    # beside the executable when OwnBook was never set.
    [string]$BookPath = ''
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
$packageName = "koi-engine-v1.1.5$platformTag"
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
$readmePath = Join-Path $repositoryRoot 'README.md'
if (-not (Test-Path -LiteralPath $readmePath -PathType Leaf)) {
    throw "README is missing: $readmePath"
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

foreach ($name in $requiredExecutables) {
    Copy-Item -LiteralPath (Join-Path $binaryDirectory $name) -Destination (Join-Path $packageDirectory $name)
}
Copy-Item -LiteralPath $readmePath -Destination (Join-Path $packageDirectory 'README.md')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSE') `
    -Destination (Join-Path $packageDirectory 'LICENSE')

$nnueIncluded = $false
$nnueGate = $null
if (-not [string]::IsNullOrWhiteSpace($NnuePath)) {
    $resolvedNnue = [System.IO.Path]::GetFullPath($NnuePath)
    if (-not (Test-Path -LiteralPath $resolvedNnue -PathType Leaf)) {
        throw "NNUE network is missing: $resolvedNnue"
    }
    $nnueDirectory = Join-Path $packageDirectory 'nnue'
    New-Item -ItemType Directory -Path $nnueDirectory -Force | Out-Null
    $nnueTarget = Join-Path $nnueDirectory 'koi.nnue'
    Copy-Item -LiteralPath $resolvedNnue -Destination $nnueTarget
    $nnueSha = Get-Sha256Hex $nnueTarget
    Set-Content -LiteralPath (Join-Path $nnueDirectory 'koi.nnue.sha256') -Value $nnueSha -Encoding ASCII
    if (-not [string]::IsNullOrWhiteSpace($NnueMetadataPath)) {
        $resolvedMetadata = [System.IO.Path]::GetFullPath($NnueMetadataPath)
        if (-not (Test-Path -LiteralPath $resolvedMetadata -PathType Leaf)) {
            throw "NNUE metadata is missing: $resolvedMetadata"
        }
        Copy-Item -LiteralPath $resolvedMetadata `
            -Destination (Join-Path $nnueDirectory 'koi.nnue.metadata.json')
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

if ($ReleasePackage) {
    if (-not $nnueIncluded) {
        throw 'A release package must bundle a gated NNUE network (-NnuePath).'
    }
    if ($null -eq $nnueGate) {
        throw 'A release package must pass -NnueGateReport with an accepted gate decision.'
    }
}

$bookIncluded = $false
if (-not [string]::IsNullOrWhiteSpace($BookPath)) {
    $resolvedBook = [System.IO.Path]::GetFullPath($BookPath)
    if (-not (Test-Path -LiteralPath $resolvedBook -PathType Leaf)) {
        throw "opening book is missing: $resolvedBook"
    }
    $bookDirectory = Join-Path $packageDirectory 'book'
    New-Item -ItemType Directory -Path $bookDirectory -Force | Out-Null
    # The engine looks for book.bin beside the executable; the package keeps it
    # in book/ with a sidecar so a user can copy it next to the binary verbatim.
    $bookTarget = Join-Path $bookDirectory 'koi.bin'
    Copy-Item -LiteralPath $resolvedBook -Destination $bookTarget
    $bookSha = Get-Sha256Hex $bookTarget
    Set-Content -LiteralPath (Join-Path $bookDirectory 'koi.bin.sha256') -Value $bookSha -Encoding ASCII
    $bookNotice = Join-Path $packageDirectory 'book/COPYING.txt'
    Set-Content -LiteralPath $bookNotice -Encoding ASCII -Value @(
        'koi.bin is a Polyglot opening book generated from lichess-org/chess-openings,',
        'released under CC0 1.0 Universal (public domain dedication).',
        'Source: https://github.com/lichess-org/chess-openings',
        'Generator: tools/book/build_book.py',
        "SHA-256: $bookSha"
    )
    $bookIncluded = $true
}

$licenseDirectory = Join-Path $packageDirectory 'licenses'
New-Item -ItemType Directory -Path $licenseDirectory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'third_party/chess-library/LICENSE') `
    -Destination (Join-Path $licenseDirectory 'chess-library-MIT.txt')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'third_party/fathom/LICENSE') `
    -Destination (Join-Path $licenseDirectory 'fathom-MIT.txt')

$engineName = "koi-engine$binarySuffix"
$avx2Name = "koi-engine-avx2$binarySuffix"
$avx512Name = "koi-engine-avx512$binarySuffix"
$nnueParagraph = if ($nnueIncluded) {
    "This package includes Koi's gated NNUE network at nnue/koi.nnue (its SHA-256 is in nnue/koi.nnue.sha256). The engine discovers it automatically at startup; set EvalFile only to override it. The classical evaluator remains the fallback."
} else {
    ''
}
$bookParagraph = if ($bookIncluded) {
    "This package includes a Polyglot opening book at book/koi.bin (CC0, generated from lichess-org/chess-openings; SHA-256 in book/koi.bin.sha256). Copy it next to the engine binary as book.bin and Koi uses it automatically: a book beside the executable is used unless the GUI sets OwnBook explicitly."
} else {
    ''
}
$launcherParagraph = if ($isMacOSHost) {
    "This package contains the native Apple Silicon (arm64) build; run $engineName directly."
} else {
@"
$engineName starts the fastest build this CPU supports: it launches the
$avx512Name or $avx2Name sibling when it is present and the CPU supports those
instructions, and otherwise runs its own baseline build in place. Set
KOI_CPU_VARIANT=generic, avx2, or avx512 to force one build.
"@
}
$developerToolsParagraph = 'Release packages ship the engine binaries and their runtime assets only. Developer tools (koi-bench, koi-replay, koi-perft, koi-eval-features) and the test suite are built from source and are not included.'
$hardwareParagraph = if ($isMacOSHost) {
    'GPU NNUE inference is experimental, disabled by default, scheduled for removal, and CUDA-only; it is not available on macOS.'
} else {
@"
Every build requires an x86-64 CPU. The AVX2 build requires AVX2 and the
AVX-512 build requires AVX-512; the baseline build has no instruction-set
requirement. The experimental GPU NNUE inference (off by default) needs an NVIDIA GPU (Pascal
sm_61 or newer) with a CUDA 12.x-capable driver and is opt-in with
KOI_GPU_NNUE=1.
"@
}
@"
Koi v1.1.5 installation

Run $engineName as a UCI engine from En Croissant or another UCI GUI.
$launcherParagraph

The optional user-supplied book.bin belongs beside $engineName. Syzygy
tablebase files are also user-supplied; configure SyzygyPath in the GUI when
they are available.
$nnueParagraph
$bookParagraph

$developerToolsParagraph

Recommended starting options:
  Hash=512
  Threads=1
  OwnBook=false
  BookFile=book.bin
  BookDepth=16
  BookRandom=false

$hardwareParagraph
"@ | Set-Content -LiteralPath (Join-Path $packageDirectory 'INSTALL.txt') -Encoding UTF8

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
    Where-Object { $_.FullName -ne (Join-Path $packageDirectory 'package.json') } |
    Sort-Object { $_.FullName.Substring($packageDirectory.Length + 1).Replace('\', '/') } |
    ForEach-Object {
        $relativePath = $_.FullName.Substring($packageDirectory.Length + 1).Replace('\', '/')
        [ordered]@{
            path = $relativePath
            size_bytes = $_.Length
            sha256 = Get-Sha256Hex $_.FullName
        }
    })
$manifest = [ordered]@{
    schema = 'koi-engine-package-v1'
    version = '1.1.5'
    platform = if ($isWindowsHost) { 'windows-x86_64' } elseif ($isMacOSHost) { 'macos-arm64' } else { 'linux-x86_64' }
    build_configuration = 'Release'
    files = $packageFiles
    book_included = [bool]$bookIncluded
    tablebases_included = $false
    nnue_included = [bool]$nnueIncluded
    nnue_gate = $nnueGate
    provenance = [ordered]@{
        source_commit = $sourceCommit
        source_dirty = [bool]$sourceDirty
        source_ref = if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_REF)) { $env:GITHUB_REF } else { $null }
        created_utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $packageDirectory 'package.json') -Encoding UTF8

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

Write-Output "package_directory=$packageDirectory"
Write-Output "archive=$archivePath"
Write-Output "archive_sha256=$archiveHash"
Write-Output "book_included=$($bookIncluded.ToString().ToLowerInvariant())"
Write-Output "nnue_included=$($nnueIncluded.ToString().ToLowerInvariant())"
Write-Output "nnue_gate_decision=$(if ($null -ne $nnueGate) { $nnueGate.decision } else { 'none' })"
Write-Output "release_package=$($ReleasePackage.ToString().ToLowerInvariant())"
