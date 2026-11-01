#!/usr/bin/env pwsh
# Linux-only OS-level allocation-failure test: run the engine under a virtual
# memory limit and force a Hash resize that cannot be satisfied.  The resize must
# report allocation_failed, preserve the working table, and still search.
param(
    [Parameter(Mandatory = $true)][string]$EnginePath
)

$ErrorActionPreference = 'Stop'

if (-not $IsLinux) {
    Write-Output 'SKIP: OS-level memory-limit test is Linux-only'
    exit 0
}

$info = [System.Diagnostics.ProcessStartInfo]::new()
$info.FileName = '/bin/bash'
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardInput = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$info.ArgumentList.Add('-c')
$info.ArgumentList.Add('ulimit -v 300000; exec "$1"')
$info.ArgumentList.Add('bash')
$info.ArgumentList.Add($EnginePath)
$proc = [System.Diagnostics.Process]::Start($info)
$outTask = $proc.StandardOutput.ReadToEndAsync()
$errTask = $proc.StandardError.ReadToEndAsync()

function Send-Line([string]$line) {
    $proc.StandardInput.WriteLine($line)
    $proc.StandardInput.Flush()
}

Send-Line 'uci'
Send-Line 'isready'
Send-Line 'setoption name Hash value 512'
Send-Line 'isready'
Send-Line 'position startpos'
Send-Line 'go depth 1'
Send-Line 'stop'
Send-Line 'quit'
$proc.StandardInput.Close()

if (-not $proc.WaitForExit(30000)) {
    try { $proc.Kill() } catch {}
    throw 'engine hung under the virtual memory limit'
}
$out = $outTask.GetAwaiter().GetResult()
$err = $errTask.GetAwaiter().GetResult()
$exitCode = $proc.ExitCode
$proc.Dispose()

$bestmoves = @($out -split "`n" | Where-Object { $_ -match '^bestmove ' })
$hashLines = @($out -split "`n" | Where-Object { $_ -match '^info string hash requested 512 MB' })
$allocationFailed = @($hashLines | Where-Object { $_ -match 'reason allocation_failed' })

if ($exitCode -ne 0) {
    throw "engine exited $exitCode under the virtual memory limit (stderr: $err)"
}
if ($bestmoves.Count -ne 1) {
    throw "expected exactly one bestmove under the memory limit, got $($bestmoves.Count)"
}
if ($hashLines.Count -ne 1) {
    throw "expected one hash-resize diagnostic under the memory limit, got $($hashLines.Count)"
}
if ($allocationFailed.Count -ne 1) {
    throw "the hash resize must report reason allocation_failed, got: $($hashLines -join ' | ')"
}
Write-Output 'PASS protocol memory-limit allocation failure'
