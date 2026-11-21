[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,
    [Parameter(Mandatory = $true)]
    [string]$PackageScript
)

$ErrorActionPreference = 'Stop'
$powerShellExecutable = (Get-Process -Id $PID).Path
$isWindowsHost = $env:OS -eq 'Windows_NT'
$isMacOSHost = -not $isWindowsHost -and
    [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::OSX)
$platformTag = if ($isWindowsHost) { '' } elseif ($isMacOSHost) { '-macos-arm64' } else { '-linux-x86_64' }
$binarySuffix = if ($isWindowsHost) { '.exe' } else { '' }
$packageName = "koi-engine-v1.2.0$platformTag"
$archivePathName = if ($isWindowsHost) { "$packageName.zip" } else { "$packageName.tar.gz" }
$outputDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ('koi-package-test-' + [guid]::NewGuid().ToString('N'))

function Get-TestSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha256.ComputeHash([System.IO.File]::ReadAllBytes($Path)))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha256.Dispose()
    }
}

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-BoundedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string[]]$InputLines = @(),
        [string]$CpuVariant = '',
        [int]$TimeoutMilliseconds = 15000,
        [string]$NnuePath = ''
    )

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    foreach ($argument in $Arguments) {
        $startInfo.ArgumentList.Add($argument)
    }
    $startInfo.WorkingDirectory = Split-Path -Parent $FilePath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.EnvironmentVariables.Remove('KOI_CPU_VARIANT')
    if (-not [string]::IsNullOrWhiteSpace($CpuVariant)) {
        $startInfo.EnvironmentVariables['KOI_CPU_VARIANT'] = $CpuVariant
    }
    # NNUE is mandatory since the HCE removal, so a packaged engine without a
    # bundled network must still be smoked with the build-tree fixture network.
    if (-not [string]::IsNullOrWhiteSpace($NnuePath)) {
        $startInfo.EnvironmentVariables['KOI_NNUE_PATH'] = $NnuePath
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) { throw "Could not start $FilePath" }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        foreach ($line in $InputLines) { $process.StandardInput.WriteLine($line) }
        $process.StandardInput.Close()
        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            if ($isWindowsHost) {
                & taskkill.exe /PID $process.Id /T /F 2>$null | Out-Null
            }
            if (-not $process.HasExited) {
                try { $process.Kill($true) } catch {
                    try { $process.Kill() } catch {}
                }
            }
            $null = $process.WaitForExit(2000)
            throw "$FilePath exceeded its $TimeoutMilliseconds ms timeout"
        }
        $process.WaitForExit()
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            StandardOutput = $stdoutTask.Result
            StandardError = $stderrTask.Result
        }
    } finally {
        $process.Dispose()
    }
}

try {
    Write-Output '[1/4] build default package and verify manifest, checksums, and provenance'
    & $powerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $PackageScript `
        -BuildDirectory $BuildDirectory -OutputDirectory $outputDirectory | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "package script exited with $LASTEXITCODE" }

    $packageDirectory = Join-Path $outputDirectory $packageName
    $manifestPath = Join-Path $outputDirectory "$packageName.package.json"
    $archivePath = Join-Path $outputDirectory $archivePathName
    $archiveHashPath = "$archivePath.sha256"
    $requiredExecutables = @("koi-engine$binarySuffix")
    if (-not $isMacOSHost) {
        $requiredExecutables = @("koi-engine$binarySuffix", "koi-engine-avx2$binarySuffix",
            "koi-engine-avx512$binarySuffix")
    }
    $requiredFiles = @($requiredExecutables + @('book.bin', 'LICENSE'))
    foreach ($required in $requiredFiles) {
        Assert-True (Test-Path -LiteralPath (Join-Path $packageDirectory $required) -PathType Leaf) "package is missing $required"
    }
    $actualPackagePaths = @(Get-ChildItem -LiteralPath $packageDirectory -File -Recurse |
        ForEach-Object { $_.FullName.Substring($packageDirectory.Length + 1).Replace('\', '/') } |
        Sort-Object)
    $expectedPackagePaths = @($requiredFiles | ForEach-Object { $_.Replace('\', '/') } | Sort-Object)
    Assert-True (($actualPackagePaths -join "`n") -ceq ($expectedPackagePaths -join "`n")) "release package must contain strictly executables, book.bin, and LICENSE; found: $($actualPackagePaths -join ', ')"
    foreach ($unexpected in @('README.md', 'INSTALL.txt', 'package.json', 'licenses', 'book', 'nnue')) {
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $packageDirectory $unexpected))) "clean release package must not include $unexpected"
    }
    $developerTools = @("koi-bench$binarySuffix", "koi-replay$binarySuffix",
        "koi-perft$binarySuffix", "koi-eval-features$binarySuffix")
    foreach ($tool in $developerTools) {
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $packageDirectory $tool))) "release package must not ship developer tool $tool"
    }
    Assert-True (Test-Path -LiteralPath $manifestPath -PathType Leaf) 'external package manifest was not produced beside the archive'
    Assert-True (Test-Path -LiteralPath $archivePath -PathType Leaf) 'package archive was not produced'
    Assert-True (Test-Path -LiteralPath $archiveHashPath -PathType Leaf) 'archive SHA-256 sidecar was not produced'

    if ($isWindowsHost) {
        foreach ($exeName in $requiredExecutables) {
            $exePath = Join-Path $packageDirectory $exeName
            $versionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($exePath)
            Assert-True ($versionInfo.ProductName -ceq 'Koi Chess Engine') "$exeName ProductName metadata is missing: '$($versionInfo.ProductName)'"
            Assert-True ($versionInfo.CompanyName -ceq 'Koi Chess Engine Contributors') "$exeName CompanyName metadata is missing: '$($versionInfo.CompanyName)'"
            Assert-True ($versionInfo.OriginalFilename -ceq $exeName) "$exeName OriginalFilename metadata mismatch: '$($versionInfo.OriginalFilename)'"
            Assert-True (-not [string]::IsNullOrWhiteSpace($versionInfo.FileDescription)) "$exeName FileDescription metadata is missing"
            Assert-True ($versionInfo.FileVersion -match '^1\.2\.0(\.|$)') "$exeName FileVersion metadata mismatch: '$($versionInfo.FileVersion)'"
        }
    }

    $manifestText = Get-Content -LiteralPath $manifestPath -Raw
    $manifest = $manifestText | ConvertFrom-Json
    Assert-True ($manifest.schema -ceq 'koi-engine-package-v1') 'package manifest schema is incorrect'
    Assert-True ($manifest.version -ceq '1.2.0') 'package manifest version is not 1.2.0'
    $expectedPlatform = if ($isWindowsHost) { 'windows-x86_64' } elseif ($isMacOSHost) { 'macos-arm64' } else { 'linux-x86_64' }
    Assert-True ($manifest.platform -ceq $expectedPlatform) 'manifest platform does not match the host'
    Assert-True ($manifest.build_configuration -ceq 'Release') 'manifest build configuration is not Release'
    Assert-True ($manifest.provenance.source_commit -match '^[0-9a-f]{40,64}$') 'manifest source commit is missing or invalid'
    $expectedCommit = $env:GITHUB_SHA
    if ($expectedCommit -notmatch '^[0-9a-f]{40,64}$') {
        $expectedCommit = (& git rev-parse HEAD).Trim()
    }
    Assert-True ($manifest.provenance.source_commit -ceq $expectedCommit) 'manifest source commit does not match the checkout'
    Assert-True ($manifestText -match '"created_utc"\s*:\s*"[^"]+Z"') 'manifest creation time is not UTC'
    Assert-True ($manifest.provenance.source_dirty -is [bool]) 'manifest source dirty flag is missing'
    Assert-True ($manifest.book_included -eq $true) 'manifest must state that the bundled CC0 book is included'
    Assert-True ($manifest.book_sha256 -ceq (Get-TestSha256 -Path (Join-Path $packageDirectory 'book.bin'))) 'manifest book SHA-256 mismatch'
    Assert-True ($manifest.tablebases_included -eq $false) 'manifest must state that tablebases are excluded'

    $manifestFiles = @($manifest.files)
    Assert-True ($manifestFiles.Count -eq $requiredFiles.Count) 'manifest must checksum every payload file'
    $expectedManifestPaths = @($requiredFiles | ForEach-Object { $_.Replace('\', '/') } | Sort-Object)
    $actualManifestPaths = @($manifestFiles | ForEach-Object { [string]$_.path } | Sort-Object)
    Assert-True (($expectedManifestPaths -join "`n") -ceq ($actualManifestPaths -join "`n")) 'manifest file list does not match package payloads'
    foreach ($file in $manifestFiles) {
        $relativePath = $file.path.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
        $payloadPath = Join-Path $packageDirectory $relativePath
        Assert-True (Test-Path -LiteralPath $payloadPath -PathType Leaf) "manifest references missing file $($file.path)"
        Assert-True ((Get-TestSha256 -Path $payloadPath) -ceq $file.sha256) "manifest SHA-256 does not match $($file.path)"
        Assert-True ((Get-Item -LiteralPath $payloadPath).Length -eq $file.size_bytes) "manifest size does not match $($file.path)"
    }

    $archiveHash = Get-TestSha256 -Path $archivePath
    Assert-True ($manifest.archive.name -ceq $archivePathName) 'manifest archive name mismatch'
    Assert-True ($manifest.archive.sha256 -ceq $archiveHash) 'manifest archive SHA-256 mismatch'
    Assert-True ($manifest.archive.size_bytes -eq (Get-Item -LiteralPath $archivePath).Length) 'manifest archive size mismatch'
    $sidecar = (Get-Content -LiteralPath $archiveHashPath -Raw).Trim()
    Assert-True ($sidecar -match "^$archiveHash\s+\*?$([regex]::Escape($archivePathName))$") 'archive SHA-256 sidecar does not match the archive'

    Write-Output '[2/4] extract archive and run replay/engine UCI smoke'
    $extractionDirectory = Join-Path $outputDirectory 'extracted'
    New-Item -ItemType Directory -Path $extractionDirectory -Force | Out-Null
    if ($isWindowsHost) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [System.IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $extractionDirectory)
    } else {
        & tar -xzf $archivePath -C $extractionDirectory
        if ($LASTEXITCODE -ne 0) { throw "tar extraction failed with exit code $LASTEXITCODE" }
    }

    $actualExtractedPaths = @(Get-ChildItem -LiteralPath $extractionDirectory -File -Recurse |
        ForEach-Object { $_.FullName.Substring($extractionDirectory.Length + 1).Replace('\', '/') } |
        Sort-Object)
    Assert-True (($actualExtractedPaths -join "`n") -ceq ($expectedPackagePaths -join "`n")) "extracted archive file list mismatch: $($actualExtractedPaths -join ', ')"
    foreach ($tool in $developerTools) {
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $extractionDirectory $tool))) "extracted release archive must not ship developer tool $tool"
    }
    $replayPath = Join-Path $BuildDirectory "koi-replay$binarySuffix"
    Assert-True (Test-Path -LiteralPath $replayPath -PathType Leaf) 'build tree must provide koi-replay for move-legality validation'
    $replayOutput = @(& $replayPath 'startpos' 'moves' 'g1f3' 'g8f6' 'f3g1' 'f6g8' 'g1f3' 'g8f6' 'f3g1' 'f6g8')
    if ($LASTEXITCODE -ne 0) { throw "extracted replay smoke failed with exit code $LASTEXITCODE" }
    Assert-True ($replayOutput -contains 'legal 1') 'extracted replay smoke did not report a legal position'
    Assert-True ($replayOutput -contains 'result 1/2-1/2') 'extracted replay smoke did not report the repetition draw'

    $enginePath = Join-Path $extractionDirectory "koi-engine$binarySuffix"
    $fixtureNetPath = Join-Path $BuildDirectory 'koi.nnue'
    Assert-True (Test-Path -LiteralPath $fixtureNetPath -PathType Leaf) 'build tree must provide the generated fixture network'
    $fixtureNet = (Resolve-Path -LiteralPath $fixtureNetPath).Path
    $uciTranscript = @('uci', 'isready', 'position startpos', 'go depth 2', 'stop', 'quit')
    foreach ($variant in @('', 'generic')) {
        $label = if ([string]::IsNullOrWhiteSpace($variant)) { 'automatic' } else { $variant }
        $smoke = Invoke-BoundedProcess -FilePath $enginePath -InputLines $uciTranscript -CpuVariant $variant -NnuePath $fixtureNet
        Assert-True ($smoke.ExitCode -eq 0) "$label extracted engine exited with $($smoke.ExitCode)"
        Assert-True ([string]::IsNullOrWhiteSpace($smoke.StandardError)) "$label extracted engine wrote to stderr: $($smoke.StandardError)"
        $smokeLines = @($smoke.StandardOutput -split "`r?`n" | Where-Object { $_ -ne '' })
        Assert-True ($smokeLines -contains 'uciok') "$label extracted engine did not complete the UCI handshake"
        Assert-True ($smokeLines -contains 'readyok') "$label extracted engine did not complete the readiness handshake"
        $bestmoveLines = @($smokeLines | Where-Object { $_ -match '^bestmove\s+' })
        Assert-True ($bestmoveLines.Count -eq 1) "$label extracted engine produced $($bestmoveLines.Count) bestmove lines"
        Assert-True ($bestmoveLines[0] -match '^bestmove\s+([a-h][1-8][a-h][1-8][nbrq]?)$') "$label extracted engine produced an invalid bestmove line"
        $bestmove = $Matches[1]
        $replay = Invoke-BoundedProcess -FilePath $replayPath -Arguments @('startpos', 'moves', $bestmove) -TimeoutMilliseconds 5000
        Assert-True ($replay.ExitCode -eq 0) "$label extracted replay exited with $($replay.ExitCode)"
        Assert-True ([string]::IsNullOrWhiteSpace($replay.StandardError)) "$label extracted replay wrote to stderr: $($replay.StandardError)"
        Assert-True (@($replay.StandardOutput -split "`r?`n") -contains 'legal 1') "$label bestmove is not legal from startpos: $bestmove"
    }

    Write-Output '[3/4] verify flat bundled NNUE layout and external provenance manifest'
    $netSourceDirectory = Join-Path $outputDirectory 'net-source'
    New-Item -ItemType Directory -Path $netSourceDirectory -Force | Out-Null
    $netSource = Join-Path $netSourceDirectory 'candidate.nnue'
    [System.IO.File]::WriteAllBytes($netSource, [byte[]](1..64))
    $metadataSource = Join-Path $netSourceDirectory 'candidate.metadata.json'
    Set-Content -LiteralPath $metadataSource -Value '{"schema":"test"}' -Encoding UTF8
    $netOutputDirectory = Join-Path $outputDirectory 'with-net'
    & $powerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $PackageScript `
        -BuildDirectory $BuildDirectory -OutputDirectory $netOutputDirectory `
        -NnuePath $netSource -NnueMetadataPath $metadataSource | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "package script with a network exited with $LASTEXITCODE" }
    $netPackage = Join-Path $netOutputDirectory $packageName
    $netTarget = Join-Path $netPackage 'koi.nnue'
    Assert-True (Test-Path -LiteralPath $netTarget -PathType Leaf) 'package is missing flat koi.nnue'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $netPackage 'nnue'))) 'clean release package must place koi.nnue flat beside binaries without nnue/ directory'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $netPackage 'koi.nnue.sha256'))) 'clean release package must not include koi.nnue.sha256 inside the archive'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $netPackage 'koi.nnue.metadata.json'))) 'clean release package must not include koi.nnue.metadata.json inside the archive'
    Assert-True ((Get-TestSha256 -Path $netTarget) -ceq (Get-TestSha256 -Path $netSource)) 'bundled network differs from the source network'
    $netManifestPath = Join-Path $netOutputDirectory "$packageName.package.json"
    $netManifest = Get-Content -LiteralPath $netManifestPath -Raw | ConvertFrom-Json
    Assert-True ($netManifest.nnue_included -eq $true) 'manifest must state that the network is included'
    Assert-True ($netManifest.nnue_metadata_sha256 -ceq (Get-TestSha256 -Path $metadataSource)) 'manifest must record external metadata SHA-256'
    $netPaths = @($netManifest.files | ForEach-Object { [string]$_.path })
    Assert-True ($netPaths -contains 'koi.nnue') 'manifest file list is missing flat koi.nnue'

    $flatSmokeDir = Join-Path $outputDirectory 'flat-smoke'
    & $powerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $PackageScript `
        -BuildDirectory $BuildDirectory -OutputDirectory $flatSmokeDir `
        -NnuePath $fixtureNet | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "package script with fixture network exited with $LASTEXITCODE" }
    $flatSmokeEngine = Join-Path (Join-Path $flatSmokeDir $packageName) "koi-engine$binarySuffix"
    $flatSmoke = Invoke-BoundedProcess -FilePath $flatSmokeEngine -InputLines $uciTranscript
    Assert-True ($flatSmoke.ExitCode -eq 0) "flat koi.nnue auto-discovery failed with $($flatSmoke.ExitCode)"
    Assert-True (@($flatSmoke.StandardOutput -split "`r?`n") -contains 'uciok') 'flat koi.nnue package did not complete UCI handshake without KOI_NNUE_PATH'
    Assert-True ((@($flatSmoke.StandardOutput -split "`r?`n" | Where-Object { $_ -match '^bestmove\s+' })).Count -eq 1) 'flat koi.nnue package did not return one bestmove'

    Write-Output '[4/4] verify NNUE gate report acceptance and rejection enforcement'
    $gateReport = Join-Path $netSourceDirectory 'nnue-gate.json'
    [ordered]@{
        schema = 'koi-nnue-gate-test-v1'
        decision = 'accept'
        candidate_sha256 = Get-TestSha256 -Path $netSource
    } | ConvertTo-Json | Set-Content -LiteralPath $gateReport -Encoding UTF8
    $gatedOutputDirectory = Join-Path $outputDirectory 'with-gated-net'
    & $powerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $PackageScript `
        -BuildDirectory $BuildDirectory -OutputDirectory $gatedOutputDirectory `
        -NnuePath $netSource -NnueMetadataPath $metadataSource `
        -NnueGateReport $gateReport -ReleasePackage | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "package script with a gate report exited with $LASTEXITCODE" }
    $gatedManifest = Get-Content -LiteralPath (Join-Path $gatedOutputDirectory "$packageName.package.json") -Raw |
        ConvertFrom-Json
    Assert-True ($gatedManifest.nnue_gate.decision -ceq 'accept') 'manifest must record the NNUE gate decision'
    Assert-True (Test-Path -LiteralPath (Join-Path (Join-Path $gatedOutputDirectory $packageName) 'koi.nnue') -PathType Leaf) 'gated package is missing flat koi.nnue'

    $badGate = Join-Path $netSourceDirectory 'nnue-gate-rejected.json'
    [ordered]@{
        schema = 'koi-nnue-gate-test-v1'
        decision = 'reject'
        candidate_sha256 = Get-TestSha256 -Path $netSource
    } | ConvertTo-Json | Set-Content -LiteralPath $badGate -Encoding UTF8
    $badOutputDirectory = Join-Path $outputDirectory 'with-rejected-net'
    & $powerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $PackageScript `
        -BuildDirectory $BuildDirectory -OutputDirectory $badOutputDirectory `
        -NnuePath $netSource -NnueGateReport $badGate 2>$null | Out-Null
    Assert-True ($LASTEXITCODE -ne 0) 'packaging must refuse a network whose gate report rejected it'

    $unbundledOutputDirectory = Join-Path $outputDirectory 'release-without-net'
    & $powerShellExecutable -NoProfile -ExecutionPolicy Bypass -File $PackageScript `
        -BuildDirectory $BuildDirectory -OutputDirectory $unbundledOutputDirectory `
        -ReleasePackage 2>$null | Out-Null
    Assert-True ($LASTEXITCODE -ne 0) 'a release package must refuse to ship without a gated network'

    Write-Output 'PASS v1.2.0 release package manifest, checksums, provenance, and extracted archive smoke'
} finally {
    if (Test-Path -LiteralPath $outputDirectory) {
        Remove-Item -LiteralPath $outputDirectory -Recurse -Force
    }
}
