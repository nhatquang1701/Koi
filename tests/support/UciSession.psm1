# UciSession.psm1
#
# Shared UCI process-session helpers for the Koi integration tests.
#
# The UCI integration scripts used to each carry their own copy of the engine
# launch / read / shutdown plumbing.  Import this module instead:
#
#   Import-Module ([System.IO.Path]::GetFullPath(
#       (Join-Path $PSScriptRoot '../../support/UciSession.psm1'))) -Force
#
# Session objects returned by Start-UciSession expose:
#   Process    : the System.Diagnostics.Process running the engine
#   StderrTask : a pending ReadToEndAsync() task for the engine's stderr
#   Lines      : the transcript accumulated by Read-UciLine/Complete-UciSession
#
# The read timeout honours $env:KOI_UCI_TIMEOUT_MS (milliseconds, default
# 15000).  Every process started here is tracked in the module so a caller's
# trap handler can call Stop-AllUciSessions and never orphan an engine.

Import-Module ([System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '../../tools/stability/EtaSupport.psm1'))) -Force

$script:UciTimeoutMilliseconds = if ($env:KOI_UCI_TIMEOUT_MS) {
    [int]$env:KOI_UCI_TIMEOUT_MS
} else {
    15000
}

# A hard upper bound for graceful "quit" handling during session teardown.
$script:UciShutdownMilliseconds = 5000

$script:KoiSessions = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()

$script:SearchInfoLinePattern =
    'info depth [1-9][0-9]* seldepth [0-9]+ multipv [1-9][0-9]*( currmove [a-h][1-8][a-h][1-8][nbrq]?( currmovenumber [1-9][0-9]*)?)?( score (cp|mate) -?[0-9]+( (lowerbound|upperbound))?( wdl [0-9]+ [0-9]+ [0-9]+)?)? nodes [0-9]+ nps [0-9]+ hashfull [0-9]+ time [0-9]+ pv( [a-h][1-8][a-h][1-8][nbrq]?)*( tbhits [0-9]+)?'

$script:UciTranscriptLinePattern =
    "^(id |option |info string .*|$($script:SearchInfoLinePattern)`$)"

function Set-UciTimeoutMilliseconds {
    param(
        [Parameter(Mandatory = $true)]
        [int]$Milliseconds
    )

    if ($Milliseconds -lt 100) {
        throw "Timeout must be at least 100 ms, got $Milliseconds."
    }
    $script:UciTimeoutMilliseconds = $Milliseconds
}

function Stop-ProcessTreeSafely {
    param([System.Diagnostics.Process]$Process)

    if ($null -eq $Process) {
        return
    }
    try {
        if (-not $Process.HasExited) {
            try { $Process.Kill($true) } catch {
                try { $Process.Kill() } catch { }
            }
            $null = $Process.WaitForExit(2000)
        }
    } catch [System.ObjectDisposedException], [System.InvalidOperationException] {
    }
}

function Prune-ExitedSessions {
    for ($index = $script:KoiSessions.Count - 1; $index -ge 0; --$index) {
        $candidate = $script:KoiSessions[$index]
        $remove = $false
        if ($null -eq $candidate) {
            $remove = $true
        } else {
            try {
                $remove = $candidate.HasExited
            } catch [System.ObjectDisposedException], [System.InvalidOperationException] {
                $remove = $true
            }
        }
        if ($remove) {
            $script:KoiSessions.RemoveAt($index)
        }
    }
}

function Start-UciSession {
    <#
    .SYNOPSIS
        Start an engine process and return a UCI session object.
    .DESCRIPTION
        Launches $Executable with redirected stdin/stdout/stderr, registers the
        process for trap-based cleanup, and returns the session object consumed
        by the other helpers in this module.
    #>
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,
        [string]$WorkingDirectory = '',
        [string[]]$Arguments = @()
    )

    Prune-ExitedSessions

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    foreach ($argument in $Arguments) {
        $startInfo.ArgumentList.Add($argument)
    }
    if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        $startInfo.WorkingDirectory = $WorkingDirectory
    }
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $startInfo.StandardErrorEncoding = [System.Text.Encoding]::UTF8

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Unable to start engine at '$Executable'."
    }
    $script:KoiSessions.Add($process)

    return [pscustomobject]@{
        Process = $process
        StderrTask = $process.StandardError.ReadToEndAsync()
        Lines = [System.Collections.Generic.List[string]]::new()
    }
}

function Stop-AllUciSessions {
    <#
    .SYNOPSIS
        Kill every engine process tree started through Start-UciSession.
    .DESCRIPTION
        Intended for a script-level trap so a failed assertion can never leave a
        running koi-engine.exe behind.  Already exited processes are ignored.
    #>
    foreach ($tracked in $script:KoiSessions) {
        Stop-ProcessTreeSafely $tracked
    }
    Prune-ExitedSessions
}

function Send-UciCommand {
    <#
    .SYNOPSIS
        Write one UCI command line to the engine's stdin and flush it.
    #>
    param(
        [Parameter(Mandatory = $true)]
        $Session,
        [Parameter(Mandatory = $true)]
        [string]$Command
    )

    $Session.Process.StandardInput.WriteLine($Command)
    $Session.Process.StandardInput.Flush()
}

function Stop-TimedOutSession {
    <#
    .SYNOPSIS
        Kill a session process tree that exceeded its timeout and rethrow the failure.
    #>
    param(
        [Parameter(Mandatory = $true)]
        $Session,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    Stop-ProcessTreeSafely $Session.Process
    throw $Message
}

function Read-UciLine {
    <#
    .SYNOPSIS
        Read one stdout line, appending it to the session transcript.
    .DESCRIPTION
        Waits up to $env:KOI_UCI_TIMEOUT_MS (default 15000) for a line.  A
        timeout kills the engine through Stop-TimedOutSession; a closed stdout
        raises a terminating error.  The returned line is also appended to
        $Session.Lines so Complete-UciSession can return the whole transcript
        without double-counting it.
    #>
    param(
        [Parameter(Mandatory = $true)]
        $Session,
        [string]$Description = 'engine output'
    )

    $readTask = $Session.Process.StandardOutput.ReadLineAsync()
    if (-not $readTask.Wait($script:UciTimeoutMilliseconds)) {
        Stop-TimedOutSession $Session "Timed out waiting for $Description."
    }

    $line = $readTask.GetAwaiter().GetResult()
    if ($null -eq $line) {
        throw "koi-engine closed stdout while waiting for $Description."
    }
    $Session.Lines.Add($line)
    return $line
}

function Read-UciUntil {
    <#
    .SYNOPSIS
        Read lines until $Predicate accepts one, then return that line.
    .DESCRIPTION
        With -ValidateUciOutput, any skipped line that is not a recognised UCI
        transcript line (id/option/info) fails the caller.  Without it, skipped
        lines are ignored exactly as the older permissive waiter did.
    #>
    param(
        [Parameter(Mandatory = $true)]
        $Session,
        [Parameter(Mandatory = $true)]
        [scriptblock]$Predicate,
        [string]$Description = 'engine output',
        [switch]$ValidateUciOutput
    )

    while ($true) {
        $line = Read-UciLine $Session $Description
        if (& $Predicate $line) {
            return $line
        }
        if ($ValidateUciOutput -and $line -notmatch $script:UciTranscriptLinePattern) {
            throw "Invalid UCI output while waiting for $Description`: $line"
        }
    }
}

function Complete-UciSession {
    <#
    .SYNOPSIS
        Drain the session, shut the engine down and return the transcript.
    .DESCRIPTION
        Captures any remaining stdout, optionally sends "quit", and waits for
        the process to exit.  A non-zero exit code or any stderr output fails
        the caller.  Returns the entire accumulated transcript ($Session.Lines),
        which is why read helpers must not count those lines twice.
    #>
    param(
        [Parameter(Mandatory = $true)]
        $Session,
        [bool]$SendQuit
    )

    $stdoutTailTask = $Session.Process.StandardOutput.ReadToEndAsync()
    if ($SendQuit -and -not $Session.Process.HasExited) {
        Send-UciCommand $Session 'quit'
    }
    $Session.Process.StandardInput.Close()

    if (-not $Session.Process.WaitForExit($script:UciTimeoutMilliseconds)) {
        Stop-TimedOutSession $Session "koi-engine did not exit within $($script:UciTimeoutMilliseconds) ms."
    }

    $tail = $stdoutTailTask.GetAwaiter().GetResult()
    foreach ($line in @($tail -split "`r?`n" | Where-Object { $_.Length -ne 0 })) {
        $Session.Lines.Add($line)
    }
    $diagnostics = $Session.StderrTask.GetAwaiter().GetResult()

    $exitCode = $Session.Process.ExitCode
    $null = $script:KoiSessions.Remove($Session.Process)
    if ($exitCode -ne 0) {
        throw "koi-engine exited with $($exitCode): $diagnostics"
    }
    if ($diagnostics.Length -ne 0) {
        throw "koi-engine wrote diagnostics for a valid transcript: $diagnostics"
    }

    return @($Session.Lines)
}

function Stop-UciSession {
    <#
    .SYNOPSIS
        Best-effort cleanup of a session without asserting on its exit state.
    .DESCRIPTION
        Sends "quit" when the engine is still running, escalates to a kill if it
        does not exit promptly, then drains stderr and disposes the process.
        Never throws, so it is safe inside a finally block.
    #>
    param(
        [Parameter(Mandatory = $true)]
        $Session
    )

    if ($null -eq $Session) {
        return
    }
    try {
        if (-not $Session.Process.HasExited) {
            try { Send-UciCommand $Session 'quit' } catch { }
            try { $Session.Process.StandardInput.Close() } catch { }
            if (-not $Session.Process.WaitForExit($script:UciShutdownMilliseconds)) {
                Stop-ProcessTreeSafely $Session.Process
            }
        }
    } catch [System.ObjectDisposedException] {
    }
    $null = $script:KoiSessions.Remove($Session.Process)
    try { $null = $Session.StderrTask.GetAwaiter().GetResult() } catch { }
    try { $Session.Process.Dispose() } catch { }
}

function Test-SearchInfo {
    <#
    .SYNOPSIS
        Return $true when $Line is a well-formed search info line.
    #>
    param(
        [Parameter(Mandatory = $true)]
        [string]$Line
    )

    # UCI permits an info line without score. Koi uses that form for a
    # selective estimate with no proven bound direction while retaining PV.
    return $Line -match "^$($script:SearchInfoLinePattern)`$"
}

$script:ProgressStartedUtc = $null
$script:ProgressTotal = 0

function Write-TestProgress {
    param(
        [Parameter(Mandatory = $true, Position = 0)]
        [int]$Step,
        [Parameter(Mandatory = $true, Position = 1)]
        [int]$Total,
        [Parameter(Mandatory = $true, Position = 2)]
        [string]$Label
    )
    if ($null -eq $script:ProgressStartedUtc -or $Step -le 1 -or $Total -ne $script:ProgressTotal) {
        $script:ProgressStartedUtc = [DateTime]::UtcNow
        $script:ProgressTotal = $Total
    }
    Write-Output (Format-StepProgress -Step $Step -Total $Total -Label $Label -StartedUtc $script:ProgressStartedUtc)
}

Export-ModuleMember -Function Start-UciSession, Set-UciTimeoutMilliseconds, Stop-AllUciSessions,
    Send-UciCommand, Stop-TimedOutSession, Read-UciLine, Read-UciUntil, Complete-UciSession,
    Stop-UciSession, Test-SearchInfo, Write-TestProgress
