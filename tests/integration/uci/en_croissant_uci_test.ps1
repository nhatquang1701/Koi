param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'

Import-Module ([System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../support/UciSession.psm1'))) -Force

$session = Start-UciSession -Executable $EnginePath
try {
    Write-TestProgress 1 4 'UCI handshake and stopped analysis search'
    Send-UciCommand $session 'uci'
    if ((Read-UciUntil -Session $session -Description 'uciok' -ValidateUciOutput -Predicate { param($line) $line -ceq 'uciok' }) -cne 'uciok') {
        throw 'UCI handshake did not complete.'
    }

    Send-UciCommand $session 'isready'
    if ((Read-UciUntil -Session $session -Description 'initial readyok' -ValidateUciOutput -Predicate { param($line) $line -ceq 'readyok' }) -cne 'readyok') {
        throw 'Initial readiness check did not complete.'
    }

    Send-UciCommand $session 'setoption name Hash value 64'
    Send-UciCommand $session 'setoption name Threads value 1'
    Send-UciCommand $session 'setoption name uci_analysemode value true'
    Send-UciCommand $session 'setoption name multipv value 3'
    Send-UciCommand $session 'position startpos moves e2e4 e7e5 g1f3'
    Send-UciCommand $session 'go depth 2'
    Send-UciCommand $session 'stop'
    $firstBestmove = Read-UciUntil -Session $session -Description 'bestmove after stopped analysis' -ValidateUciOutput -Predicate { param($line) $line -match '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$' }
    if ($firstBestmove -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
        throw "Stopped analysis emitted an invalid bestmove: $firstBestmove"
    }

    Send-UciCommand $session 'isready'
    if ((Read-UciUntil -Session $session -Description 'readyok after stopped analysis' -ValidateUciOutput -Predicate { param($line) $line -ceq 'readyok' }) -cne 'readyok') {
        throw 'Stopped analysis did not fence at readyok.'
    }

    Write-TestProgress 2 4 'MultiPV=3 ranked search'
    Send-UciCommand $session 'position startpos moves e2e4 e7e5 g1f3'
    Send-UciCommand $session 'go depth 2'
    $secondBestmoves = [System.Collections.Generic.List[string]]::new()
    $multiPvRanks = [System.Collections.Generic.HashSet[int]]::new()
    while ($secondBestmoves.Count -eq 0) {
        $line = Read-UciLine $session 'MultiPV bestmove'
        if ($line -match '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
            $secondBestmoves.Add($line)
        } elseif ($line -match '^info string ') {
            # Informational diagnostics (hash/NNUE notices) are valid UCI.
            $null = $line
        } elseif ($line -match '^info depth [1-9][0-9]* seldepth [0-9]+ multipv ([1-9][0-9]*)(?: currmove [a-h][1-8][a-h][1-8][nbrq]?(?: currmovenumber [1-9][0-9]*)?)?(?: score (?:cp|mate) -?[0-9]+(?: (?:lowerbound|upperbound))?(?: wdl [0-9]+ [0-9]+ [0-9]+)?)? nodes [0-9]+ nps [0-9]+ hashfull [0-9]+ time [0-9]+ pv(?: [a-h][1-8][a-h][1-8][nbrq]?)*(?: tbhits [0-9]+)?$') {
            $null = $multiPvRanks.Add([int]$Matches[1])
        } else {
            throw "Invalid MultiPV output: $line"
        }
    }
    if ($secondBestmoves.Count -ne 1 -or -not ($multiPvRanks.Contains(1) -and $multiPvRanks.Contains(2) -and $multiPvRanks.Contains(3))) {
        throw "MultiPV search did not emit ranks 1-3 and exactly one bestmove."
    }

    Write-TestProgress 3 4 'infinite analysis readiness and interim progress'
    Send-UciCommand $session 'position startpos moves e2e4 e7e5'
    Send-UciCommand $session 'go infinite'
    Send-UciCommand $session 'isready'
    if ((Read-UciUntil -Session $session -Description 'readyok during infinite analysis' -ValidateUciOutput -Predicate { param($line) $line -ceq 'readyok' }) -cne 'readyok') {
        throw 'Infinite analysis did not remain ready.'
    }

    # A long iteration must refresh its published progress instead of leaving a
    # GUI frozen on the last completed depth: read lines until a repeated depth
    # reports strictly more nodes than its previous line for that depth.
    $interimNodesByDepth = @{}
    $interimRefreshSeen = $false
    while (-not $interimRefreshSeen) {
        $line = Read-UciLine $session 'interim progress during infinite analysis'
        if (-not (Test-SearchInfo $line)) {
            throw "Invalid interim-progress output: $line"
        }
        if ($line -match '^info depth ([1-9][0-9]*) .* nodes ([0-9]+) ') {
            $depth = [int]$Matches[1]
            $nodes = [System.UInt64]$Matches[2]
            if ($interimNodesByDepth.ContainsKey($depth) -and
                $nodes -gt $interimNodesByDepth[$depth]) {
                $interimRefreshSeen = $true
            }
            $interimNodesByDepth[$depth] = $nodes
        }
    }
    Send-UciCommand $session 'stop'
    $thirdBestmove = Read-UciUntil -Session $session -Description 'bestmove after infinite analysis' -ValidateUciOutput -Predicate { param($line) $line -match '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$' }
    if ($thirdBestmove -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
        throw "Infinite analysis emitted an invalid bestmove: $thirdBestmove"
    }

    Write-TestProgress 4 5 'go movetime 1000 stays strictly under 1000 ms (Threads 1 and 4)'
    foreach ($threadCount in @(1, 4)) {
        Send-UciCommand $session "setoption name Threads value $threadCount"
        foreach ($moves in @(
            'e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3 a7a6',
            'd2d4 d7d5 c2c4 e7e6 b1c3 g8f6 c1g5 f8e7 e2e3 e8g8'
        )) {
            Send-UciCommand $session "position startpos moves $moves"
            $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
            Send-UciCommand $session 'go movetime 1000'
            $bestmove = Read-UciUntil -Session $session -Description "bestmove after go movetime 1000 (Threads=$threadCount)" -ValidateUciOutput -Predicate { param($line) $line -match '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$' }
            $stopwatch.Stop()
            if ($bestmove -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
                throw "movetime 1000 search emitted an invalid bestmove: $bestmove"
            }
            if ($stopwatch.ElapsedMilliseconds -ge 1000) {
                throw "go movetime 1000 (Threads=$threadCount) took $($stopwatch.ElapsedMilliseconds) ms (>= 1000 ms GUI deadline)."
            }
        }
    }
    Send-UciCommand $session 'setoption name Threads value 1'

    Write-TestProgress 5 5 'shutdown and transcript verification'
    Send-UciCommand $session 'register later'
    Send-UciCommand $session 'debug on'
    $null = Complete-UciSession $session $true

    $bestmoves = @($session.Lines | Where-Object { $_ -like 'bestmove *' })
    if ($bestmoves.Count -ne 7) {
        throw "En Croissant transcript emitted $($bestmoves.Count) bestmoves; expected exactly seven."
    }
    foreach ($line in $session.Lines) {
        if ($line -notmatch '^(id |option |uciok$|readyok$|info )' -and $line -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
            throw "Transcript contains non-UCI output: $line"
        }
    }
    Write-Output 'En Croissant UCI transcript passed.'
} finally {
    Stop-UciSession $session
}
