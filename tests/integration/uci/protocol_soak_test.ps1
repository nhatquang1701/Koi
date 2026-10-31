param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath,
    [int]$Rounds = 60
)

# Reliability soak: repeatedly reconfigure resources (Threads/Hash), run short
# searches, and bounce the game state.  The engine must keep a single protocol
# writer, answer isready throughout, emit exactly one legal bestmove per search,
# keep stderr empty, keep memory bounded, and shut down cleanly.  This covers the
# resource-churn surface that the fast fuzz corpus intentionally does not.
$ErrorActionPreference = 'Stop'
$TimeoutMilliseconds = if ($env:KOI_UCI_TIMEOUT_MS) {
    [int]$env:KOI_UCI_TIMEOUT_MS
} else {
    30000
}

Import-Module ([System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '../../support/UciSession.psm1'))) -Force

trap {
    Stop-AllUciSessions
    break
}

function Send-AndExpect($Session, [string]$Command, [string]$Expected, [string]$Description) {
    Send-UciCommand $Session $Command
    $line = Read-UciUntil $Session { param($candidate) $candidate -eq $Expected } $Description -ValidateUciOutput
    if ($line -ne $Expected) { throw "expected '$Expected' while waiting for $Description" }
    return $line
}

function Send-Bestmove($Session, [string]$Description) {
    Send-UciCommand $Session 'stop'
    $line = Read-UciUntil $Session { param($candidate) $candidate -match '^bestmove ' } $Description -ValidateUciOutput
    if ($line -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
        throw "invalid bestmove while waiting for $Description`: $line"
    }
    return $line
}

$session = Start-UciSession -Executable $EnginePath
try {
    $session.Process.Refresh()
    [void](Send-AndExpect $session 'uci' 'uciok' 'handshake')
    [void](Send-AndExpect $session 'isready' 'readyok' 'initial readiness')

    $threadsCeiling = [Math]::Max(1, [Math]::Min(4, [Environment]::ProcessorCount))
    $hashSizes = @(8, 16, 32, 64)
    $positions = @(
        'position startpos',
        'position startpos moves e2e4 e7e5 g1f3',
        'position startpos moves d2d4 d7d5 c2c4 e7e6',
        'position fen r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3'
    )

    # Warm up so the first hash materialisation does not count as growth.
    Send-UciCommand $session $positions[0]
    Send-UciCommand $session 'go depth 2'
    [void](Send-Bestmove $session 'warmup bestmove')
    [void](Send-AndExpect $session 'isready' 'readyok' 'warmup readiness')
    $session.Process.Refresh()
    $baselineBytes = $session.Process.WorkingSet64
    $bestmoves = 1
    $rounds = $Rounds
    $started = [DateTime]::UtcNow

    for ($round = 0; $round -lt $rounds; $round++) {
        $threads = ($round % $threadsCeiling) + 1
        $hash = $hashSizes[$round % $hashSizes.Count]
        Send-UciCommand $session "setoption name Threads value $threads"
        Send-UciCommand $session "setoption name Hash value $hash"
        Send-UciCommand $session $positions[$round % $positions.Count]
        $search = switch ($round % 3) {
            0 { 'go depth 3' }
            1 { 'go movetime 25' }
            default { 'go nodes 3000' }
        }
        Send-UciCommand $session $search
        $earlyBestmove = $null
        if (($round % 10) -eq 0) {
            # Guarantee a published depth line reaches the transcript when the
            # search runs long enough.  A very short movetime search may legally
            # complete before any depth line is emitted (observed once in 50k
            # rounds after Threads/Hash churn), so accept that legal bestmove as
            # the completion instead of waiting for a line that will not come.
            $earlyLine = Read-UciUntil $session {
                param($candidate) ($candidate -match '^info depth [1-9]') -or ($candidate -match '^bestmove ')
            } "round $round depth line or early completion" -ValidateUciOutput
            if ($earlyLine -match '^bestmove ') {
                $earlyBestmove = $earlyLine
            }
        }
        if ($null -ne $earlyBestmove) {
            if ($earlyBestmove -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
                throw "invalid early bestmove in round $round`: $earlyBestmove"
            }
        } else {
            [void](Send-Bestmove $session "round $round bestmove")
        }
        $bestmoves++

        # isready must stay responsive between every reconfiguration and search.
        Send-UciCommand $session 'isready'
        [void](Read-UciUntil $session { param($candidate) $candidate -eq 'readyok' } "round $round readiness" -ValidateUciOutput)

        if (($round + 1) % 10 -eq 0) {
            $session.Process.Refresh()
            $currentBytes = $session.Process.WorkingSet64
            $growth = $currentBytes - $baselineBytes
            if ($growth -gt 256MB) {
                throw "memory growth $([Math]::Round($growth / 1MB, 1)) MB exceeds the soak bound after round $round"
            }
            Write-Output ("soak round {0}/{1}: rss {2:N1} MB (growth {3:N1} MB)" -f ($round + 1), $rounds,
                ($currentBytes / 1MB), ($growth / 1MB))
        }
        if (($round + 1) % 20 -eq 0) {
            Send-UciCommand $session 'ucinewgame'
            [void](Send-AndExpect $session 'isready' 'readyok' "round $round ucinewgame readiness")
        }
    }

    Send-UciCommand $session 'quit'
    $transcript = Complete-UciSession $session $false

    # GUIs display the score from `info` lines; an analysis/search must publish
    # at least one scored line (a score-less stream reads as 0 cp in En
    # Croissant's analysis panel and other UCI GUIs).
    $scoredLines = @($transcript | Where-Object {
        $_ -match '^info depth [1-9][0-9]* ' -and
        $_ -match ' score (cp|mate) -?[0-9]+'
    })
    if ($scoredLines.Count -eq 0) {
        throw 'the soak searches published no scored info lines'
    }
    $elapsed = ([DateTime]::UtcNow - $started).TotalSeconds
    Write-Output ("PASS protocol soak: rounds={0} bestmoves={1} scored={2} elapsed={3:N1}s" -f $rounds, $bestmoves, $scoredLines.Count, $elapsed)
}
finally {
    Stop-AllUciSessions
}
