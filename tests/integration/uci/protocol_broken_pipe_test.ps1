param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

# GUI-disconnect robustness: a crashed GUI closes our stdout while the engine
# is searching.  Protocol writes must fail silently (no SIGPIPE termination on
# POSIX, no crash on Windows), and `stop`/`quit` must still drive a clean exit.
$ErrorActionPreference = 'Stop'
$TimeoutMilliseconds = 15000
$script:Processes = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()

trap {
    foreach ($process in $script:Processes) {
        if (-not $process.HasExited) {
            try { $process.Kill() } catch {}
        }
        $process.Dispose()
    }
    break
}

function Start-EngineProcess {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $EnginePath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "unable to start $EnginePath"
    }
    $script:Processes.Add($process)
    return $process
}

function Complete-BrokenPipeCase($Process, [string]$Label) {
    if (-not $Process.WaitForExit($TimeoutMilliseconds)) {
        try { $Process.Kill() } catch {}
        throw "$Label did not exit after a closed stdout pipe"
    }
    $diagnostics = $Process.StandardError.ReadToEnd()
    if ($Process.ExitCode -ne 0) {
        throw "$Label exited with $($Process.ExitCode) after the GUI disconnected"
    }
    if ($diagnostics.Length -ne 0) {
        throw "$Label wrote stderr after the GUI disconnected: $diagnostics"
    }
}

# 1. Mid-search disconnect.
$midSearch = Start-EngineProcess
$midSearch.StandardInput.WriteLine('uci')
$midSearch.StandardInput.WriteLine('isready')
$midSearch.StandardInput.WriteLine('position startpos')
$midSearch.StandardInput.WriteLine('go infinite')
$midSearch.StandardInput.Flush()
Start-Sleep -Milliseconds 600
$midSearch.StandardOutput.Close()
$midSearch.StandardInput.WriteLine('stop')
$midSearch.StandardInput.WriteLine('quit')
$midSearch.StandardInput.Flush()
$midSearch.StandardInput.Close()
Complete-BrokenPipeCase $midSearch 'mid-search disconnect'
$midSearch.Dispose()

# 2. Disconnect before the handshake even completes.
$early = Start-EngineProcess
$early.StandardOutput.Close()
$early.StandardInput.WriteLine('uci')
$early.StandardInput.WriteLine('isready')
$early.StandardInput.WriteLine('quit')
$early.StandardInput.Flush()
$early.StandardInput.Close()
Complete-BrokenPipeCase $early 'early disconnect'
$early.Dispose()

# 3. Closed stderr read end: trigger a diagnostic (invalid FEN) and require a
# clean exit; POSIX SIGPIPE must stay ignored and Windows must fail closed.
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $EnginePath
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $false
$startInfo.RedirectStandardError = $true
$stderrClosed = [System.Diagnostics.Process]::new()
$stderrClosed.StartInfo = $startInfo
if (-not $stderrClosed.Start()) {
    throw "unable to start $EnginePath for the stderr test"
}
$script:Processes.Add($stderrClosed)
$stderrClosed.StandardError.Close()
$stderrClosed.StandardInput.WriteLine('uci')
$stderrClosed.StandardInput.WriteLine('position fen definitely-not-a-fen')
$stderrClosed.StandardInput.WriteLine('quit')
$stderrClosed.StandardInput.Flush()
$stderrClosed.StandardInput.Close()
if (-not $stderrClosed.WaitForExit($TimeoutMilliseconds)) {
    try { $stderrClosed.Kill() } catch {}
    throw 'engine hung after a written diagnostic with stderr closed'
}
if ($stderrClosed.ExitCode -ne 0) {
    throw "engine exited with $($stderrClosed.ExitCode) after a written diagnostic with stderr closed"
}
$stderrClosed.Dispose()

Write-Output 'PASS protocol broken pipe'
