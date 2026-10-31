param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

# Option-churn race surface: resource and asset options (Hash, Threads,
# MultiPV, EvalFile, BookFile, SyzygyPath) may arrive while a search is
# running.  Each change must stop/join/suppress the old generation without
# leaving a late bestmove, and the next search must still complete with exactly
# one legal bestmove; a malformed asset swap must be rejected cleanly.
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
    [void](Read-UciUntil $Session { param($line) $line -eq $Expected } $Description -ValidateUciOutput)
}

function Start-Search($Session) {
    Send-UciCommand $Session 'position startpos'
    Send-UciCommand $Session 'go infinite'
    [void](Read-UciUntil $Session { param($line) $line -match '^info depth [1-9]' } 'active search depth' -ValidateUciOutput)
}

function Stop-Expect-Suppressed($Session, [string]$Description) {
    # An option change stops and suppresses the active generation: isready must
    # answer without any bestmove in between (Read-UciUntil validates the
    # transcript lines it skips, so a stray bestmove fails the test).
    Send-AndExpect $Session 'isready' 'readyok' $Description
}

function Search-Expect-Bestmove($Session, [string]$Position, [string]$Description) {
    Send-UciCommand $Session "position $Position"
    Send-UciCommand $Session 'go depth 2'
    Send-UciCommand $Session 'stop'
    $line = Read-UciUntil $Session { param($candidate) $candidate -match '^bestmove ' } $Description -ValidateUciOutput
    if ($line -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
        throw "invalid bestmove for $Description`: $line"
    }
    return $line
}

$scratchRoot = Join-Path (Split-Path -Parent $EnginePath) ("option-churn-assets-" + $PID)
New-Item -ItemType Directory -Path $scratchRoot -Force | Out-Null
$badNet = Join-Path $scratchRoot 'bad.nnue'
[System.IO.File]::WriteAllBytes($badNet, [byte[]]::new(32))
$missing = Join-Path $scratchRoot 'definitely-missing'

$session = Start-UciSession -Executable $EnginePath
try {
    Send-AndExpect $session 'uci' 'uciok' 'handshake'
    Send-AndExpect $session 'isready' 'readyok' 'initial readiness'

    # 1. Hash resize during an active search.
    Start-Search $session
    Send-UciCommand $session 'setoption name Hash value 32'
    Stop-Expect-Suppressed $session 'hash-change readiness'
    [void](Search-Expect-Bestmove $session 'startpos moves e2e4' 'hash-change replacement search')

    # 2. Threads and MultiPV changes during an active search.
    Start-Search $session
    Send-UciCommand $session 'setoption name Threads value 2'
    Stop-Expect-Suppressed $session 'threads-change readiness'
    Send-UciCommand $session 'setoption name MultiPV value 2'
    Stop-Expect-Suppressed $session 'multipv-change readiness'
    Send-UciCommand $session 'setoption name MultiPV value 1'
    Stop-Expect-Suppressed $session 'multipv-restore readiness'
    [void](Search-Expect-Bestmove $session 'startpos moves d2d4' 'threads/multipv replacement search')

    # 3. Malformed EvalFile swapped in during a search.
    Start-Search $session
    Send-UciCommand $session "setoption name EvalFile value $badNet"
    Send-AndExpect $session 'isready' 'readyok' 'bad-evalfile readiness'
    [void](Search-Expect-Bestmove $session 'startpos moves g1f3' 'post-bad-evalfile search')

    # 4. BookFile/SyzygyPath churn during a search.
    Start-Search $session
    Send-UciCommand $session ("setoption name BookFile value " + (Join-Path $missing 'book.bin'))
    Stop-Expect-Suppressed $session 'bookfile-change readiness'
    Send-UciCommand $session "setoption name SyzygyPath value $missing"
    Stop-Expect-Suppressed $session 'syzygypath-change readiness'
    [void](Search-Expect-Bestmove $session 'startpos moves c2c4' 'post-asset-churn search')

    # 5. Burst of Hash resizes while one search is active.
    Start-Search $session
    foreach ($hash in @(8, 16, 24, 48, 64)) {
        Send-UciCommand $session "setoption name Hash value $hash"
    }
    Stop-Expect-Suppressed $session 'hash-burst readiness'
    [void](Search-Expect-Bestmove $session 'startpos moves b1c3' 'post-hash-burst search')

    Send-UciCommand $session 'quit'
    $transcript = Complete-UciSession $session $false

    $bestmoves = @($transcript | Where-Object { $_ -match '^bestmove ' })
    if ($bestmoves.Count -ne 5) {
        throw "option churn must complete exactly its five replacement searches, got $($bestmoves.Count) bestmoves"
    }
    $rejected = @($transcript | Where-Object { $_ -match '^info string EvalFile rejected' })
    if ($rejected.Count -ne 1) {
        throw 'a malformed EvalFile swap must be rejected exactly once'
    }
    Write-Output 'PASS protocol option churn'
}
finally {
    Stop-AllUciSessions
    Remove-Item -LiteralPath $scratchRoot -Recurse -Force -ErrorAction SilentlyContinue
}
