param(
    [Parameter(Mandatory = $true)]
    [string]$BenchPath
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $BenchPath -PathType Leaf)) {
    throw "koi-bench executable is missing: $BenchPath"
}

$scratchDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("koi-hce-bench-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratchDirectory -Force | Out-Null
$profilePath = Join-Path $scratchDirectory 'hce-profile.json'

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $BenchPath
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.Arguments = "--hce --depth 3 --profile-json `"$profilePath`""
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $startInfo
if (-not $process.Start()) {
    throw 'Unable to start koi-bench --hce.'
}
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
if (-not $process.WaitForExit(180000)) {
    $process.Kill()
    $process.WaitForExit()
    throw 'koi-bench --hce --depth 3 did not exit within 180000 ms.'
}
$stdout = $stdoutTask.GetAwaiter().GetResult()
$stderr = $stderrTask.GetAwaiter().GetResult()
if ($process.ExitCode -ne 0) {
    throw "koi-bench --hce exited with $($process.ExitCode): $stderr"
}
if ($stderr.Length -ne 0) {
    throw "koi-bench --hce wrote stderr: $stderr"
}

# Every built-in position prints `match 0|1`; enforce a regression threshold on
# the deterministic Threads=1 depth-3 suite rather than an exact count.
$matchLines = @($stdout -split "`r?`n" | Where-Object { $_ -match ' match [01]$' })
if ($matchLines.Count -lt 64) {
    throw "koi-bench --hce must report a match result for every suite position (got $($matchLines.Count))."
}
$matches = @($matchLines | Where-Object { $_ -match ' match 1$' }).Count
if ($matches -lt 60) {
    throw "HCE depth-3 tactical match rate regressed: $matches/64 (require at least 60)."
}

if (-not (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
    throw 'koi-bench --hce did not write its profile JSON.'
}
$profile = Get-Content -LiteralPath $profilePath -Raw | ConvertFrom-Json
if ($profile.evaluator -cne 'hce' -or $profile.hce.active -ne $true -or
    $profile.positions.Count -ne 64) {
    throw 'HCE profile must report evaluator=hce with 64 measured positions.'
}
