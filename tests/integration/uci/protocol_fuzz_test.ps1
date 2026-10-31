param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

# Deterministic adversarial UCI corpus.  Every transcript is seeded or literal
# so a failure is reproducible; the checks pin the controller contract rather
# than implementation details:
#   * a clean transcript exits 0, writes no stderr, and emits only well-formed
#     UCI lines;
#   * a completed search emits exactly one legal bestmove;
#   * oversized lines are rejected with one protocol-valid diagnostic and the
#     session keeps serving;
#   * isready answers during an infinite search;
#   * malformed commands never crash or hang the engine.
$ErrorActionPreference = 'Stop'
$TimeoutMilliseconds = if ($env:KOI_UCI_TIMEOUT_MS) {
    [int]$env:KOI_UCI_TIMEOUT_MS
} else {
    20000
}

Import-Module ([System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '../../support/UciSession.psm1'))) -Force

trap {
    Stop-AllUciSessions
    break
}

$script:LineCount = 0

function Invoke-UciTranscript([string]$Transcript, [string]$Label) {
    $session = Start-UciSession -Executable $EnginePath
    $stdoutTask = $session.Process.StandardOutput.ReadToEndAsync()
    $session.Process.StandardInput.Write($Transcript)
    $session.Process.StandardInput.Close()

    if (-not $session.Process.WaitForExit($TimeoutMilliseconds)) {
        Stop-TimedOutSession $session "koi-engine hung on fuzz case '$Label'."
    }

    $output = $stdoutTask.GetAwaiter().GetResult()
    $diagnostics = $session.StderrTask.GetAwaiter().GetResult()
    if ($session.Process.ExitCode -ne 0) {
        throw "koi-engine exited with $($session.Process.ExitCode) on fuzz case '$Label': $diagnostics"
    }
    return [pscustomobject]@{
        Label = $Label
        Lines = @($output -split "`r?`n" | Where-Object { $_.Length -ne 0 })
        Diagnostics = $diagnostics
    }
}

function Assert-ProtocolGrammar($Result) {
    foreach ($line in $Result.Lines) {
        $valid = $line -match '^(id [a-z]|option name |uciok$|readyok$|info )' -or
            $line -match '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$'
        if (-not $valid) {
            throw "fuzz case '$($Result.Label)' wrote a non-protocol stdout line: $line"
        }
    }
}

function Assert-CleanDiagnostics($Result) {
    if ($Result.Diagnostics.Length -ne 0) {
        throw "fuzz case '$($Result.Label)' wrote stderr for a clean transcript: $($Result.Diagnostics)"
    }
}

function Count-Lines($Result, [string]$Pattern) {
    return @($Result.Lines | Where-Object { $_ -match $Pattern }).Count
}

function Assert-OneLegalBestmove($Result) {
    $bestmoves = @($Result.Lines | Where-Object {
        $_ -match '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$'
    })
    if ($bestmoves.Count -ne 1) {
        throw "fuzz case '$($Result.Label)' must emit exactly one legal bestmove, got $($bestmoves.Count): $($bestmoves -join ' | ')"
    }
}

# 1. Baseline: handshake, options, one completed search, clean shutdown.
$baseline = Invoke-UciTranscript "uci`nisready`nsetoption name Threads value 1`nsetoption name Hash value 8`nposition startpos`ngo depth 2`nstop`nquit`n" 'baseline'
Assert-ProtocolGrammar $baseline
Assert-CleanDiagnostics $baseline
Assert-OneLegalBestmove $baseline
if ((Count-Lines $baseline '^uciok$') -ne 1 -or (Count-Lines $baseline '^readyok$') -ne 1) {
    throw 'baseline fuzz case must emit exactly one uciok and one readyok'
}

# 2. Oversized line: consumed, rejected once, session keeps serving.
$oversizedLine = 'x' * (1MB + 1)
$oversized = Invoke-UciTranscript "$oversizedLine`nisready`nuci`nquit`n" 'oversized-line'
Assert-ProtocolGrammar $oversized
Assert-CleanDiagnostics $oversized
if ((Count-Lines $oversized '^info string command rejected: line exceeds ') -ne 1) {
    throw 'an oversized line must be rejected with exactly one protocol-valid diagnostic'
}
if ((Count-Lines $oversized '^readyok$') -ne 1 -or (Count-Lines $oversized '^uciok$') -ne 1) {
    throw 'the session must keep serving after an oversized line'
}

# 3. A line exactly at the limit is still accepted.
$atLimit = 'uci' + (' ' * (1MB - 3))
$limit = Invoke-UciTranscript "$atLimit`nisready`nquit`n" 'line-at-limit'
Assert-ProtocolGrammar $limit
Assert-CleanDiagnostics $limit
if ((Count-Lines $limit '^uciok$') -ne 1 -or (Count-Lines $limit '^readyok$') -ne 1) {
    throw 'a line exactly at the size limit must still parse and answer'
}

# 4. EOF during an infinite search: clean shutdown, no late bestmove.
$eof = Invoke-UciTranscript "uci`nposition startpos`ngo infinite`n" 'eof-during-search'
Assert-ProtocolGrammar $eof
Assert-CleanDiagnostics $eof
if ((Count-Lines $eof '^bestmove ') -ne 0) {
    throw 'EOF cancellation must suppress the active search bestmove'
}

# 5. Malformed-command battery: no crash, no hang, protocol-valid output.
$malformed = Invoke-UciTranscript @"
uci
position fen bad fen
position startpos moves e2e5
position fen 8/8/8/8/8/8/8/8 w - - 0 1
go depth -1
go movetime abc
go nodes 999999999999999999999
go depth 2147483647
setoption name Hash value -1
setoption name Threads value 0
setoption name Threads value 9999
setoption name MultiPV value 0
setoption name MultiPV value 999
setoption name EvalFile value /nonexistent/does-not-exist.nnue
setoption name NoSuchOption value 1
setoption
setoption name
unknown command here
debug maybe
stop
isready
position startpos
go depth 2
stop
quit
"@ 'malformed-battery'
Assert-ProtocolGrammar $malformed
if ((Count-Lines $malformed '^uciok$') -ne 1 -or (Count-Lines $malformed '^readyok$') -ne 1) {
    throw 'malformed battery must still complete the handshake and answer isready'
}
foreach ($line in @($malformed.Diagnostics -split "`r?`n" | Where-Object { $_.Length -ne 0 })) {
    if ($line -notmatch '^UCI position error: ') {
        throw "malformed battery produced an unexpected stderr line: $line"
    }
}

# 6. isready must answer while an infinite search is running.
$interactive = Start-UciSession -Executable $EnginePath
Send-UciCommand $interactive 'uci'
[void](Read-UciUntil $interactive { param($line) $line -eq 'uciok' } 'uciok' -ValidateUciOutput)
Send-UciCommand $interactive 'isready'
[void](Read-UciUntil $interactive { param($line) $line -eq 'readyok' } 'readyok' -ValidateUciOutput)
Send-UciCommand $interactive 'setoption name Threads value 1'
Send-UciCommand $interactive 'position startpos'
Send-UciCommand $interactive 'go infinite'
[void](Read-UciUntil $interactive { param($line) $line -match '^info depth ' } 'search info' -ValidateUciOutput)
Send-UciCommand $interactive 'isready'
[void](Read-UciUntil $interactive { param($line) $line -eq 'readyok' } 'readyok during search' -ValidateUciOutput)
Send-UciCommand $interactive 'stop'
$bestmoveLine = Read-UciUntil $interactive { param($line) $line -match '^bestmove ' } 'bestmove after stop' -ValidateUciOutput
if ($bestmoveLine -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
    throw "isready/stop lifecycle produced an invalid bestmove: $bestmoveLine"
}
Send-UciCommand $interactive 'quit'
[void](Complete-UciSession $interactive $false)

# 7. Seeded random sequences: deterministic, exit 0, no stderr, valid grammar.
$random = [System.Random]::new(20261031)
$valid = @(
    'uci', 'isready', 'stop', 'ucinewgame',
    'position startpos', 'position startpos moves e2e4 e7e5',
    'setoption name Hash value 8', 'setoption name Threads value 1',
    'setoption name MultiPV value 1', 'setoption name Ponder value false',
    'go depth 1', 'go depth 2', 'go movetime 5', 'go nodes 1000'
)
# Random sequences stay stderr-free by design: malformed `position` commands
# are covered by the dedicated malformed battery above (they intentionally write
# a `UCI position error:` diagnostic to stderr).
$malformedTokens = @(
    '', ' ', 'go depth', 'go depth 999999999999',
    'go movetime -5', 'go nodes 0',
    'setoption name Hash', 'setoption name Threads value -3',
    'setoption name Unknown value 1', 'ponderhit', 'debug maybe', 'quit later'
)
for ($case = 0; $case -lt 60; $case++) {
    $builder = [System.Text.StringBuilder]::new()
    $commandCount = $random.Next(2, 12)
    for ($index = 0; $index -lt $commandCount; $index++) {
        $token = if ($random.NextDouble() -lt 0.6) {
            $valid[$random.Next($valid.Count)]
        } else {
            $malformedTokens[$random.Next($malformedTokens.Count)]
        }
        [void]$builder.AppendLine($token)
    }
    [void]$builder.AppendLine('isready')
    [void]$builder.AppendLine('quit')
    $result = Invoke-UciTranscript $builder.ToString() "random-$case"
    Assert-ProtocolGrammar $result
    Assert-CleanDiagnostics $result
}


# 8. Lifecycle storms: the controller must stay single-writer and exactly-once
# while searches are replaced, suppressed and stopped.
function New-InteractiveSession([string]$Label) {
    $session = Start-UciSession -Executable $EnginePath
    Send-UciCommand $session 'uci'
    [void](Read-UciUntil $session { param($line) $line -eq 'uciok' } "$Label uciok" -ValidateUciOutput)
    Send-UciCommand $session 'isready'
    [void](Read-UciUntil $session { param($line) $line -eq 'readyok' } "$Label readyok" -ValidateUciOutput)
    return $session
}

function Assert-LegalBestmoveLine([string]$Line, [string]$Label) {
    if ($Line -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
        throw "$Label produced an invalid bestmove line: $Line"
    }
}

# Search interruption storm: stop/isready/position/hash changes interleaved with
# live searches; every emitted bestmove must be legal and the shutdown clean.
$storm = New-InteractiveSession 'interruption-storm'
Send-UciCommand $storm 'position startpos'
Send-UciCommand $storm 'go infinite'
[void](Read-UciUntil $storm { param($line) $line -match '^info depth ' } 'interruption-storm search' -ValidateUciOutput)
Send-UciCommand $storm 'isready'
[void](Read-UciUntil $storm { param($line) $line -eq 'readyok' } 'interruption-storm readyok' -ValidateUciOutput)
Send-UciCommand $storm 'stop'
$stormBest = Read-UciUntil $storm { param($line) $line -match '^bestmove ' } 'interruption-storm bestmove' -ValidateUciOutput
Assert-LegalBestmoveLine $stormBest 'interruption-storm'
Send-UciCommand $storm 'go infinite'
[void](Read-UciUntil $storm { param($line) $line -match '^info depth ' } 'interruption-storm second search' -ValidateUciOutput)
Send-UciCommand $storm 'position startpos'
Send-UciCommand $storm 'ucinewgame'
Send-UciCommand $storm 'setoption name Hash value 16'
Send-UciCommand $storm 'isready'
[void](Read-UciUntil $storm { param($line) $line -eq 'readyok' } 'interruption-storm second readyok' -ValidateUciOutput)
Send-UciCommand $storm 'position startpos'
Send-UciCommand $storm 'go depth 2'
Send-UciCommand $storm 'stop'
$stormBest2 = Read-UciUntil $storm { param($line) $line -match '^bestmove ' } 'interruption-storm second bestmove' -ValidateUciOutput
Assert-LegalBestmoveLine $stormBest2 'interruption-storm'
Send-UciCommand $storm 'quit'
$stormLines = Complete-UciSession $storm $false
$stormBestmoves = @($stormLines | Where-Object { $_ -match '^bestmove ' })
foreach ($line in $stormBestmoves) { Assert-LegalBestmoveLine $line 'interruption-storm transcript' }
if ($stormBestmoves.Count -ne 2) {
    throw "interruption storm must emit exactly its two completed-search bestmoves, got $($stormBestmoves.Count)"
}

# Ponder -> ponderhit -> stop: exactly one legal bestmove and a clean shutdown.
$ponder = New-InteractiveSession 'ponder-lifecycle'
Send-UciCommand $ponder 'position startpos'
Send-UciCommand $ponder 'go ponder wtime 60000 btime 60000'
Send-UciCommand $ponder 'isready'
[void](Read-UciUntil $ponder { param($line) $line -eq 'readyok' } 'ponder readyok' -ValidateUciOutput)
Send-UciCommand $ponder 'ponderhit'
[void](Read-UciUntil $ponder { param($line) $line -match '^info depth ' } 'ponderhit converted search' -ValidateUciOutput)
Send-UciCommand $ponder 'stop'
$ponderBest = Read-UciUntil $ponder { param($line) $line -match '^bestmove ' } 'ponder bestmove' -ValidateUciOutput
Assert-LegalBestmoveLine $ponderBest 'ponder-lifecycle'
Send-UciCommand $ponder 'quit'
$ponderLines = Complete-UciSession $ponder $false
$ponderBestmoves = @($ponderLines | Where-Object { $_ -match '^bestmove ' })
if ($ponderBestmoves.Count -ne 1) {
    throw "ponder lifecycle must emit exactly one bestmove, got $($ponderBestmoves.Count)"
}

# Hash resize and ucinewgame during a live search must suppress the old
# generation without a late bestmove.
$resize = New-InteractiveSession 'hash-resize-during-search'
Send-UciCommand $resize 'position startpos'
Send-UciCommand $resize 'go infinite'
[void](Read-UciUntil $resize { param($line) $line -match '^info depth ' } 'resize search' -ValidateUciOutput)
Send-UciCommand $resize 'setoption name Hash value 32'
Send-UciCommand $resize 'isready'
[void](Read-UciUntil $resize { param($line) $line -eq 'readyok' } 'resize readyok' -ValidateUciOutput)
Send-UciCommand $resize 'stop'
Send-UciCommand $resize 'ucinewgame'
Send-UciCommand $resize 'position startpos'
Send-UciCommand $resize 'go depth 2'
Send-UciCommand $resize 'stop'
$resizeBest = Read-UciUntil $resize { param($line) $line -match '^bestmove ' } 'resize replacement bestmove' -ValidateUciOutput
Assert-LegalBestmoveLine $resizeBest 'hash-resize-during-search'
Send-UciCommand $resize 'quit'
$resizeLines = Complete-UciSession $resize $false
$resizeBestmoves = @($resizeLines | Where-Object { $_ -match '^bestmove ' })
if ($resizeBestmoves.Count -ne 1) {
    throw "hash-resize scenario must only complete the replacement search, got $($resizeBestmoves.Count) bestmoves"
}

# Repeated stop without a search stays quiet and does not break isready.
$stopStorm = Invoke-UciTranscript "uci`nstop`nstop`nstop`nisready`nquit`n" 'stop-storm'
Assert-ProtocolGrammar $stopStorm
Assert-CleanDiagnostics $stopStorm
if ((Count-Lines $stopStorm '^bestmove ') -ne 0) {
    throw 'an idle stop storm must not emit a bestmove'
}
if ((Count-Lines $stopStorm '^readyok$') -ne 1) {
    throw 'an idle stop storm must still answer isready'
}

# EOF while pondering: clean shutdown with no late bestmove.
$ponderEof = Invoke-UciTranscript "uci`nisready`nposition startpos`ngo ponder wtime 60000 btime 60000`n" 'ponder-eof'
Assert-ProtocolGrammar $ponderEof
Assert-CleanDiagnostics $ponderEof
if ((Count-Lines $ponderEof '^bestmove ') -ne 0) {
    throw 'EOF during a ponder search must suppress the late bestmove'
}

Write-Output 'PASS protocol fuzz corpus'

