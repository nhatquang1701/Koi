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

Write-Output 'PASS protocol broken pipe'
