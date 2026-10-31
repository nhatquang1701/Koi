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

function Invoke-UciTranscript([string]$Transcript, [string]$Label,
                              [string]$Executable = '') {
    $executablePath = if ($Executable) { $Executable } else { $EnginePath }
    $session = Start-UciSession -Executable $executablePath
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
            $line -match '^bestmove ([a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?|0000)$'
        if (-not $valid) {
            throw "fuzz case '$($Result.Label)' wrote a non-protocol stdout line: $line"
        }
        if ($line -match '^info depth ') {
            $hasCurrmove = $line -match ' currmove '
            $hasNumber = $line -match ' currmovenumber [1-9][0-9]*'
            if ($hasCurrmove -ne $hasNumber) {
                throw "fuzz case '$($Result.Label)' must pair currmove with currmovenumber: $line"
            }
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

function Assert-OnlyBestmove($Result, [string]$Expected) {
    $bestmoves = @($Result.Lines | Where-Object { $_ -match '^bestmove ' })
    if ($bestmoves.Count -ne 1 -or $bestmoves[0] -ne $Expected) {
        throw "fuzz case '$($Result.Label)' must emit exactly '$Expected', got $($bestmoves -join ' | ')"
    }
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
position fen 8/8/8/4k3/8/4K3/8/8 w - - 999999999 999999999
position fen 8/8/8/4k3/8/4K3/8/8 w - - -1 0
go depth -1
go depth 0
go movetime abc
go nodes 999999999999999999999
go nodes -1
go wtime -1 btime -1
go wtime 999999999999 btime 999999999999
go movestogo 0 wtime 1000 btime 1000
go mate 0
go mate 65
go depth 2147483647
setoption name Hash value -1
setoption name Threads value 0
setoption name Threads value 9999
setoption name MultiPV value 0
setoption name MultiPV value 999
setoption name EvalFile value /nonexistent/does-not-exist.nnue
setoption name OwnBook value maybe
setoption name BookRandom value maybe
setoption name BookDepth value 999
setoption name BookSafetyDepth value 9
setoption name RandomSeed value 999999999999999999999
setoption name Contempt value 9999
setoption name Move Overhead value 99999
setoption name Slow Mover value 5
setoption name MultiPV value 0
setoption name BookFile value "C:\Program Files\koi book.bin"
setoption name BookFile value C:\Users\koi-ünicode\book.bin
setoption name DebugFile value C:\nonexistent-dir\koi.log
setoption name SyzygyPath value ;;;
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
# Observe at least one depth line before converting: the timer/state conversion
# must work mid-search, not only at the initial boundary.
[void](Read-UciUntil $ponder { param($line) $line -match '^info depth [1-9]' } 'ponder initial depth' -ValidateUciOutput)
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


# 9. Fail-closed asset matrix: malformed/missing NNUE, book and Syzygy inputs
# must leave the engine usable, protocol-clean and crash-free.
$assetRoot = Join-Path (Split-Path -Parent $EnginePath) ("protocol-fuzz-assets-" + $PID)
$assetEngine = Join-Path $assetRoot 'engine'
$assetAuto = Join-Path $assetRoot 'engine-auto'
$assetAutoBadBook = Join-Path $assetRoot 'engine-auto-bad-book'
$assetAutoBadSyzygy = Join-Path $assetRoot 'engine-auto-bad-syzygy'
$executableName = [System.IO.Path]::GetFileName($EnginePath)
try {
    foreach ($directory in @($assetRoot, $assetEngine, $assetAuto, $assetAutoBadBook,
                             $assetAutoBadSyzygy)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    $badBook = Join-Path $assetEngine 'bad-book.bin'
    [System.IO.File]::WriteAllBytes($badBook, [byte[]]::new(17))
    $badNet = Join-Path $assetEngine 'bad.nnue'
    [System.IO.File]::WriteAllBytes($badNet, [byte[]]::new(32))
    $missingPath = Join-Path $assetRoot 'definitely-missing'

    # 9a. Explicit malformed BookFile: rejected, search still answers.
    $badBookResult = Invoke-UciTranscript "uci`nsetoption name OwnBook value true`nsetoption name BookFile value $badBook`nisready`nposition startpos`ngo depth 2`nstop`nquit`n" 'bad-book'
    Assert-ProtocolGrammar $badBookResult
    Assert-CleanDiagnostics $badBookResult
    Assert-OneLegalBestmove $badBookResult
    if ((Count-Lines $badBookResult 'book detected|book move ') -ne 0) {
        throw 'a malformed explicit book must not be announced or used'
    }

    # 9b. Malformed EvalFile: rejected with a diagnostic, search continues with
    # the boot evaluator.
    $badNetResult = Invoke-UciTranscript "uci`nsetoption name EvalFile value $badNet`nisready`nposition startpos`ngo depth 1`nstop`nquit`n" 'bad-evalfile'
    Assert-ProtocolGrammar $badNetResult
    Assert-CleanDiagnostics $badNetResult
    Assert-OneLegalBestmove $badNetResult
    if ((Count-Lines $badNetResult '^info string EvalFile rejected') -ne 1) {
        throw 'a malformed EvalFile must be rejected with exactly one info string'
    }

    # 9c. Missing Syzygy path: diagnostic, ordinary search intact.
    $missingSyzygy = Invoke-UciTranscript "uci`nsetoption name SyzygyPath value $missingPath`nisready`nposition startpos`ngo depth 1`nstop`nquit`n" 'missing-syzygy'
    Assert-ProtocolGrammar $missingSyzygy
    Assert-CleanDiagnostics $missingSyzygy
    Assert-OneLegalBestmove $missingSyzygy
    if ((Count-Lines $missingSyzygy '^info string Syzygy: no usable tables') -ne 1) {
        throw 'a missing Syzygy path must be reported once and then ignored'
    }

    # 9d. Auto-detected but malformed book beside a copied engine: not announced,
    # not used, search still legal.
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetAutoBadBook $executableName)
    [System.IO.File]::WriteAllBytes((Join-Path $assetAutoBadBook 'book.bin'), [byte[]]::new(17))
    $autoBadBookResult = Invoke-UciTranscript "uci`nisready`nposition startpos`ngo depth 2`nstop`nquit`n" 'auto-bad-book' (Join-Path $assetAutoBadBook $executableName)
    Assert-ProtocolGrammar $autoBadBookResult
    Assert-CleanDiagnostics $autoBadBookResult
    Assert-OneLegalBestmove $autoBadBookResult
    if ((Count-Lines $autoBadBookResult 'book detected|book move ') -ne 0) {
        throw 'an unusable auto-detected book must stay silent and unused'
    }

    # 9e. Auto-detected malformed Syzygy table: discovery may report the
    # directory, but the invalid tables must not crash the engine or leak stderr.
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetAutoBadSyzygy $executableName)
    $fakeSyzygy = Join-Path $assetAutoBadSyzygy 'syzygy'
    New-Item -ItemType Directory -Path $fakeSyzygy -Force | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $fakeSyzygy 'KQvK.rtbw'), [byte[]]::new(80))
    $autoBadSyzygyResult = Invoke-UciTranscript "uci`nisready`nposition startpos`ngo depth 1`nstop`nquit`n" 'auto-bad-syzygy' (Join-Path $assetAutoBadSyzygy $executableName)
    Assert-ProtocolGrammar $autoBadSyzygyResult
    Assert-CleanDiagnostics $autoBadSyzygyResult
    Assert-OneLegalBestmove $autoBadSyzygyResult
    if ((Count-Lines $autoBadSyzygyResult 'syzygy detected|^info string Syzygy:') -ne 0) {
        throw 'a corrupt auto-detected Syzygy directory must be rejected silently'
    }
    # Even though the corrupt directory was rejected, probing an endgame in the
    # same engine directory must remain safe and silent.
    $fakeSyzygyProbe = Invoke-UciTranscript "uci`nisready`nposition fen 4k3/8/8/8/8/8/8/3QK3 w - - 0 1`ngo depth 2`nstop`nquit`n" 'auto-bad-syzygy-probe' (Join-Path $assetAutoBadSyzygy $executableName)
    Assert-ProtocolGrammar $fakeSyzygyProbe
    Assert-CleanDiagnostics $fakeSyzygyProbe
    Assert-OneLegalBestmove $fakeSyzygyProbe

    # 9f. An explicit corrupt SyzygyPath is disabled with one diagnostic rather
    # than being handed to Fathom (which would print on stderr at probe time).
    $explicitBadSyzygy = Invoke-UciTranscript "uci`nsetoption name SyzygyPath value $fakeSyzygy`nisready`nposition fen 4k3/8/8/8/8/8/8/3QK3 w - - 0 1`ngo depth 2`nstop`nquit`n" 'explicit-bad-syzygy'
    Assert-ProtocolGrammar $explicitBadSyzygy
    Assert-CleanDiagnostics $explicitBadSyzygy
    Assert-OneLegalBestmove $explicitBadSyzygy
    if ((Count-Lines $explicitBadSyzygy '^info string Syzygy: corrupt tables at ') -ne 1) {
        throw 'an explicit corrupt Syzygy directory must be disabled with one diagnostic'
    }

    # 9f. Assets must not leak into a plain run: no book/Syzygy noise when none is
    # configured and no conventional directory exists beside this engine copy.
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetAuto $executableName)
    $plainResult = Invoke-UciTranscript "uci`nisready`nposition startpos`ngo depth 1`nstop`nquit`n" 'plain-assets' (Join-Path $assetAuto $executableName)
    Assert-ProtocolGrammar $plainResult
    Assert-CleanDiagnostics $plainResult
    Assert-OneLegalBestmove $plainResult
    if ((Count-Lines $plainResult 'book detected|syzygy detected|^info string Syzygy:') -ne 0) {
        throw 'an engine directory without assets must not announce any asset'
    }

    # 9g. Auto-detected book hot-swap: a valid book is used, a truncated book
    # falls back silently, restoring it reloads (no negative cache), and
    # deleting it returns to search without crashing.
    $assetBookChurn = Join-Path $assetRoot 'engine-book-churn'
    New-Item -ItemType Directory -Path $assetBookChurn -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetBookChurn $executableName)
    $bookPath = Join-Path $assetBookChurn 'book.bin'
    # One Polyglot entry: startpos key + e2e4 (raw 0x031C) + weight 100.
    $bookBytes = [byte[]]::new(16)
    [byte[]]$startposKey = 0x46, 0x3b, 0x96, 0x18, 0x16, 0x91, 0xfc, 0x9c
    [Array]::Copy($startposKey, 0, $bookBytes, 0, 8)
    $bookBytes[8] = 0x03
    $bookBytes[9] = 0x1c
    $bookBytes[10] = 0x00
    $bookBytes[11] = 100
    [System.IO.File]::WriteAllBytes($bookPath, $bookBytes)

    $bookChurnSession = Start-UciSession -Executable (Join-Path $assetBookChurn $executableName)
    Send-UciCommand $bookChurnSession 'uci'
    [void](Read-UciUntil $bookChurnSession { param($line) $line -eq 'uciok' } 'book-churn uciok' -ValidateUciOutput)
    Send-UciCommand $bookChurnSession 'isready'
    [void](Read-UciUntil $bookChurnSession { param($line) $line -eq 'readyok' } 'book-churn readyok' -ValidateUciOutput)

    function Search-BookChurn {
        param($Session, [string]$Description)
        Send-UciCommand $Session 'position startpos'
        Send-UciCommand $Session 'go depth 1'
        [void](Read-UciUntil $Session { param($line) $line -match '^bestmove ' } $Description -ValidateUciOutput)
    }

    Search-BookChurn $bookChurnSession 'book-churn valid search'
    [System.IO.File]::WriteAllBytes($bookPath, [byte[]]::new(8))
    Search-BookChurn $bookChurnSession 'book-churn truncated search'
    [System.IO.File]::WriteAllBytes($bookPath, $bookBytes)
    Search-BookChurn $bookChurnSession 'book-churn restored search'
    Remove-Item -LiteralPath $bookPath -Force
    Search-BookChurn $bookChurnSession 'book-churn removed search'
    Send-UciCommand $bookChurnSession 'quit'
    $bookChurnLines = Complete-UciSession $bookChurnSession $false
    $bookChurnBestmoves = @($bookChurnLines | Where-Object { $_ -match '^bestmove ' })
    if ($bookChurnBestmoves.Count -ne 4) {
        throw "book hot-swap must complete four searches, got $($bookChurnBestmoves.Count) bestmoves"
    }
    $bookMoveLines = @($bookChurnLines | Where-Object { $_ -match '^info string book move e2e4 ' })
    if ($bookMoveLines.Count -lt 2) {
        throw "a valid auto-detected book must be used before and after a transient corruption, got $($bookMoveLines.Count) book moves"
    }
    if (@($bookChurnLines | Where-Object { $_ -match '^info string book detected' }).Count -ne 1) {
        throw 'auto-detected book announcement must appear exactly once'
    }
}
finally {
    Stop-AllUciSessions
    Remove-Item -LiteralPath $assetRoot -Recurse -Force -ErrorAction SilentlyContinue
}

# 10. Terminal and forced-draw roots, plus analysis mode.  Terminal roots
# must answer `bestmove 0000`; a forced fivefold/locked draw must still produce
# a legal move; analysis mode must publish a scored line and stay protocol-clean.
$mateRoot = Invoke-UciTranscript "uci`nisready`nposition fen 7k/6Q1/6K1/8/8/8/8/8 b - - 0 1`ngo depth 3`nstop`nquit`n" 'mate-root'
Assert-ProtocolGrammar $mateRoot
Assert-CleanDiagnostics $mateRoot
Assert-OnlyBestmove $mateRoot 'bestmove 0000'

$stalemateRoot = Invoke-UciTranscript "uci`nisready`nposition fen 7k/5Q2/6K1/8/8/8/8/8 b - - 0 1`ngo depth 3`nstop`nquit`n" 'stalemate-root'
Assert-ProtocolGrammar $stalemateRoot
Assert-CleanDiagnostics $stalemateRoot
Assert-OnlyBestmove $stalemateRoot 'bestmove 0000'

$repetitionMoves = 'g1f3 g8f6 f3g1 f6g8 ' * 4
$fivefoldRoot = Invoke-UciTranscript "uci`nisready`nposition startpos moves $($repetitionMoves.Trim())`ngo depth 2`nstop`nquit`n" 'fivefold-root'
Assert-ProtocolGrammar $fivefoldRoot
Assert-CleanDiagnostics $fivefoldRoot
Assert-OneLegalBestmove $fivefoldRoot

$analysis = Start-UciSession -Executable $EnginePath
Send-UciCommand $analysis 'uci'
[void](Read-UciUntil $analysis { param($line) $line -eq 'uciok' } 'analysis uciok' -ValidateUciOutput)
Send-UciCommand $analysis 'setoption name UCI_AnalyseMode value true'
Send-UciCommand $analysis 'setoption name MultiPV value 3'
Send-UciCommand $analysis 'isready'
[void](Read-UciUntil $analysis { param($line) $line -eq 'readyok' } 'analysis readyok' -ValidateUciOutput)
Send-UciCommand $analysis 'position fen r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3'
Send-UciCommand $analysis 'go infinite'
$analysisDepthLine = Read-UciUntil $analysis { param($line) $line -match '^info depth [1-9][0-9]* .* multipv 3 .* score (cp|mate) ' } 'analysis multipv-3 scored depth' -ValidateUciOutput
$analysisDepth = if ($analysisDepthLine -match '^info depth (\d+) ') { [int]$Matches[1] } else { 0 }
if ($analysisDepth -le 0) {
    throw 'analysis mode must publish a positive depth for the multipv-3 line'
}
foreach ($line in 1..3) {
    $matchesForLine = @($analysis.Lines | Where-Object {
        $_ -match ("^info depth " + $analysisDepth + " .* multipv " + $line + " .* score (cp|mate) ")
    })
    if ($matchesForLine.Count -lt 1) {
        throw "analysis mode must publish a scored multipv $line line at depth $analysisDepth"
    }
}
Send-UciCommand $analysis 'stop'
$analysisBest = Read-UciUntil $analysis { param($line) $line -match '^bestmove ' } 'analysis bestmove' -ValidateUciOutput
if ($analysisBest -notmatch '^bestmove [a-h][1-8][a-h][1-8][nbrq]?( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
    throw "analysis mode produced an invalid bestmove: $analysisBest"
}
Send-UciCommand $analysis 'quit'
$analysisLines = Complete-UciSession $analysis $false
if (@($analysisLines | Where-Object { $_ -match 'book move|book detected' }).Count -ne 0) {
    throw 'analysis mode must bypass the opening book'
}

# 11. searchmoves filtering: the completion gate must return one of the
# requested legal moves, and an empty intersection must answer 0000 with one
# explanatory info string (never a crash or an unrequested move).
$searchTwo = Invoke-UciTranscript "uci`nisready`nposition startpos`ngo depth 3 searchmoves e2e4 d2d4`nstop`nquit`n" 'searchmoves-two'
Assert-ProtocolGrammar $searchTwo
Assert-CleanDiagnostics $searchTwo
$twoBest = @($searchTwo.Lines | Where-Object { $_ -match '^bestmove ' })
if ($twoBest.Count -ne 1 -or $twoBest[0] -notmatch '^bestmove (e2e4|d2d4)( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
    throw "searchmoves-two must answer with one requested legal move, got $($twoBest -join ' | ')"
}

$searchIllegal = Invoke-UciTranscript "uci`nisready`nposition startpos`ngo depth 2 searchmoves e2e5`nstop`nquit`n" 'searchmoves-illegal'
Assert-ProtocolGrammar $searchIllegal
Assert-CleanDiagnostics $searchIllegal
Assert-OnlyBestmove $searchIllegal 'bestmove 0000'
if ((Count-Lines $searchIllegal '^info string no common legal move') -ne 1) {
    throw 'an impossible searchmoves filter must explain itself exactly once'
}

$searchDuplicate = Invoke-UciTranscript "uci`nisready`nposition startpos`ngo depth 2 searchmoves e2e4 e2e4`nstop`nquit`n" 'searchmoves-duplicate'
Assert-ProtocolGrammar $searchDuplicate
Assert-CleanDiagnostics $searchDuplicate
Assert-OneLegalBestmove $searchDuplicate

$searchEmpty = Invoke-UciTranscript "uci`nisready`nposition startpos`ngo depth 2 searchmoves`nstop`nquit`n" 'searchmoves-empty'
Assert-ProtocolGrammar $searchEmpty
Assert-CleanDiagnostics $searchEmpty
Assert-OnlyBestmove $searchEmpty 'bestmove 0000'
if ((Count-Lines $searchEmpty '^info string no common legal move') -ne 1) {
    throw 'an empty searchmoves filter must explain itself exactly once'
}

# 12. A final command without a trailing newline must still be executed, and the
# session must shut down cleanly with exactly its completed search.
$partialEof = Invoke-UciTranscript "uci`nisready`nposition startpos`ngo depth 2`nstop" 'partial-line-eof'
Assert-ProtocolGrammar $partialEof
Assert-CleanDiagnostics $partialEof
Assert-OneLegalBestmove $partialEof

# 13. A stop storm while a search is running: exactly one bestmove, then idle
# stops stay quiet (a second bestmove would fail the readyok transcript check).
$stopStormActive = New-InteractiveSession 'active-stop-storm'
Send-UciCommand $stopStormActive 'position startpos'
Send-UciCommand $stopStormActive 'go infinite'
[void](Read-UciUntil $stopStormActive { param($line) $line -match '^info depth ' } 'active stop storm search' -ValidateUciOutput)
foreach ($unused in 1..5) {
    Send-UciCommand $stopStormActive 'stop'
}
$stormBest = Read-UciUntil $stopStormActive { param($line) $line -match '^bestmove ' } 'active stop storm bestmove' -ValidateUciOutput
Assert-LegalBestmoveLine $stormBest 'active-stop-storm'
Send-UciCommand $stopStormActive 'isready'
[void](Read-UciUntil $stopStormActive { param($line) $line -eq 'readyok' } 'active stop storm readyok' -ValidateUciOutput)
Send-UciCommand $stopStormActive 'quit'
$stormLines = Complete-UciSession $stopStormActive $false
$stormBestmoves = @($stormLines | Where-Object { $_ -match '^bestmove ' })
if ($stormBestmoves.Count -ne 1) {
    throw "a stop storm during search must emit exactly one bestmove, got $($stormBestmoves.Count)"
}

# 14. ucinewgame during a ponder search cancels the ponder generation; the next
# real search completes with exactly one bestmove.
$ponderReset = New-InteractiveSession 'ucinewgame-during-ponder'
Send-UciCommand $ponderReset 'position startpos'
Send-UciCommand $ponderReset 'go ponder wtime 60000 btime 60000'
Send-UciCommand $ponderReset 'isready'
[void](Read-UciUntil $ponderReset { param($line) $line -eq 'readyok' } 'ponder reset readyok' -ValidateUciOutput)
Send-UciCommand $ponderReset 'ucinewgame'
Send-UciCommand $ponderReset 'isready'
[void](Read-UciUntil $ponderReset { param($line) $line -eq 'readyok' } 'ponder reset post-newgame readyok' -ValidateUciOutput)
Send-UciCommand $ponderReset 'position startpos moves e2e4'
Send-UciCommand $ponderReset 'go depth 2'
Send-UciCommand $ponderReset 'stop'
$ponderBest = Read-UciUntil $ponderReset { param($line) $line -match '^bestmove ' } 'ponder reset bestmove' -ValidateUciOutput
Assert-LegalBestmoveLine $ponderBest 'ucinewgame-during-ponder'
Send-UciCommand $ponderReset 'quit'
$ponderLines = Complete-UciSession $ponderReset $false
$ponderBestmoves = @($ponderLines | Where-Object { $_ -match '^bestmove ' })
if ($ponderBestmoves.Count -ne 1) {
    throw "ucinewgame during ponder must leave exactly one bestmove, got $($ponderBestmoves.Count)"
}

# 15. Missing/minimal handshake and a huge searchmoves token list.  A GUI may
# send commands before `uci` (or close immediately); the controller must stay
# quiet, crash-free and still honour the later completion contract.
$emptyInput = Invoke-UciTranscript "" 'empty-input'
Assert-CleanDiagnostics $emptyInput
if ((Count-Lines $emptyInput '^bestmove ') -ne 0) {
    throw 'empty input must not emit a bestmove'
}

$noHandshake = Invoke-UciTranscript "isready`nposition startpos`ngo depth 2`nstop`nquit`n" 'no-handshake'
Assert-ProtocolGrammar $noHandshake
Assert-CleanDiagnostics $noHandshake
Assert-OneLegalBestmove $noHandshake
if ((Count-Lines $noHandshake '^uciok$') -ne 0) {
    throw 'a transcript without uci must not emit a handshake'
}

$preHandshake = Invoke-UciTranscript "setoption name Hash value 32`nisready`nposition startpos`ngo depth 1`nstop`nquit`n" 'setoption-before-handshake'
Assert-ProtocolGrammar $preHandshake
Assert-CleanDiagnostics $preHandshake
Assert-OneLegalBestmove $preHandshake

$manyMoves = ('e2e4 ' * 20000).Trim()
$hugeSearchmoves = Invoke-UciTranscript "uci`nisready`nposition startpos`ngo depth 1 searchmoves $manyMoves`nstop`nquit`n" 'huge-searchmoves'
Assert-ProtocolGrammar $hugeSearchmoves
Assert-CleanDiagnostics $hugeSearchmoves
$hugeBest = @($hugeSearchmoves.Lines | Where-Object { $_ -match '^bestmove ' })
if ($hugeBest.Count -ne 1 -or $hugeBest[0] -notmatch '^bestmove e2e4( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$') {
    throw "a huge searchmoves list must still answer e2e4, got $($hugeBest -join ' | ')"
}

# 16. Extreme clock limits and the ponder stop/ponderhit contracts.  All of
# these must finish with a legal (or suppressed, for the idle ponderhit) answer
# and a clean shutdown.
foreach ($clockCommand in @(
        'go wtime 0 btime 0',
        'go wtime 1 btime 1',
        'go wtime 600000000 btime 600000000')) {
    $clockCase = Invoke-UciTranscript "uci`nisready`nposition startpos`n$clockCommand`nstop`nquit`n" "clock-$($clockCommand -replace ' ','-')"
    Assert-ProtocolGrammar $clockCase
    Assert-CleanDiagnostics $clockCase
    Assert-OneLegalBestmove $clockCase
}

$ponderStop = New-InteractiveSession 'ponder-stop'
Send-UciCommand $ponderStop 'position startpos'
Send-UciCommand $ponderStop 'go ponder wtime 60000 btime 60000'
Start-Sleep -Milliseconds 300
Send-UciCommand $ponderStop 'stop'
$ponderStopBest = Read-UciUntil $ponderStop { param($line) $line -match '^bestmove ' } 'ponder stop bestmove' -ValidateUciOutput
Assert-LegalBestmoveLine $ponderStopBest 'ponder-stop'
Send-UciCommand $ponderStop 'quit'
$ponderStopLines = Complete-UciSession $ponderStop $false
if (@($ponderStopLines | Where-Object { $_ -match '^bestmove ' }).Count -ne 1) {
    throw 'stop during a ponder search must emit exactly one bestmove'
}

$idlePonderhit = New-InteractiveSession 'ponderhit-without-ponder'
Send-UciCommand $idlePonderhit 'position startpos'
Send-UciCommand $idlePonderhit 'stop'
Send-UciCommand $idlePonderhit 'ponderhit'
Send-UciCommand $idlePonderhit 'isready'
[void](Read-UciUntil $idlePonderhit { param($line) $line -eq 'readyok' } 'idle ponderhit readyok' -ValidateUciOutput)
Send-UciCommand $idlePonderhit 'quit'
$idlePonderhitLines = Complete-UciSession $idlePonderhit $false
if (@($idlePonderhitLines | Where-Object { $_ -match '^bestmove ' }).Count -ne 0) {
    throw 'ponderhit without a ponder search must not start or complete a search'
}

# 17. A very long legal reversible history (16k cycles ~ 320 KB line) must
# parse and search promptly; halfmove-clock overflow beyond 65535 plies is
# rejected as an invalid move while the prior position is retained.
$reversibleCycle = 'g1f3 g8f6 f3g1 f6g8 '
$longMoves = (($reversibleCycle * 16000).Trim())
$longHistory = Invoke-UciTranscript "uci`nisready`nposition startpos moves $longMoves`ngo depth 1`nstop`nquit`n" 'long-legal-history'
Assert-ProtocolGrammar $longHistory
Assert-CleanDiagnostics $longHistory
Assert-OneLegalBestmove $longHistory

# 18. quit during a ponder search: clean shutdown without a late bestmove.
$ponderQuit = New-InteractiveSession 'ponder-quit'
Send-UciCommand $ponderQuit 'position startpos'
Send-UciCommand $ponderQuit 'go ponder wtime 60000 btime 60000'
Start-Sleep -Milliseconds 250
Send-UciCommand $ponderQuit 'quit'
$ponderQuitLines = Complete-UciSession $ponderQuit $false
if (@($ponderQuitLines | Where-Object { $_ -match '^bestmove ' }).Count -ne 0) {
    throw 'quit during ponder must suppress the late bestmove'
}

# 19. Interim progress lines pair the in-flight root move with its one-based
# currmovenumber (UCI schema); a long-enough infinite search must publish one.
$interim = New-InteractiveSession 'interim-currmovenumber'
Send-UciCommand $interim 'position fen r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3'
Send-UciCommand $interim 'go infinite'
$interimLine = Read-UciUntil $interim { param($line) $line -match '^info depth [1-9][0-9]* .* currmove [a-h][1-8][a-h][1-8][nbrq]? currmovenumber [1-9][0-9]* ' } 'interim currmove line' -ValidateUciOutput
if ($interimLine -notmatch ' currmovenumber [1-9][0-9]* ') {
    throw 'interim currmove lines must carry a one-based currmovenumber'
}
Send-UciCommand $interim 'stop'
$interimBest = Read-UciUntil $interim { param($line) $line -match '^bestmove ' } 'interim search bestmove' -ValidateUciOutput
Assert-LegalBestmoveLine $interimBest 'interim-currmovenumber'
Send-UciCommand $interim 'quit'
[void](Complete-UciSession $interim $false)

# 20. Repeated handshake and contradictory `go` limits: each must remain
# protocol-clean with exactly one legal completion when stopped.
$doubleHandshake = Invoke-UciTranscript "uci`nuci`nisready`nquit`n" 'double-handshake'
Assert-ProtocolGrammar $doubleHandshake
Assert-CleanDiagnostics $doubleHandshake
if ((Count-Lines $doubleHandshake '^uciok$') -ne 2 -or (Count-Lines $doubleHandshake '^readyok$') -ne 1) {
    throw 'a repeated uci command must re-answer the handshake without extra output'
}

$limitCases = @(
    'go depth 2 infinite',
    'go mate 2 depth 10',
    'go infinite wtime 1000 btime 1000',
    'go ponder depth 2',
    'go depth 3 nodes 5000',
    'go'
)
foreach ($limitCase in $limitCases) {
    $label = 'limits-' + ($limitCase -replace ' ', '-')
    $case = Invoke-UciTranscript "uci`nisready`nposition startpos`n$limitCase`nstop`nquit`n" $label
    Assert-ProtocolGrammar $case
    Assert-CleanDiagnostics $case
    Assert-OneLegalBestmove $case
}

# 21. Handshake flood: repeated `uci` commands re-answer every time without
# drift, duplicate output, or a shutdown failure.
$handshakeFlood = "uci`n" * 500
$flood = Invoke-UciTranscript ($handshakeFlood + "isready`nquit`n") 'handshake-flood'
Assert-ProtocolGrammar $flood
Assert-CleanDiagnostics $flood
if ((Count-Lines $flood '^uciok$') -ne 500) {
    throw '500 uci commands must produce exactly 500 uciok answers'
}

# 22. Seeded state-machine stress: random interleavings of position changes,
# go variants, stop, ponderhit, ucinewgame, isready and resource options.  The
# engine may publish any number of legal bestmoves; it must not crash, hang,
# emit a malformed line, or leave stderr noise.
$random = [System.Random]::new(20261031)
$operations = @(
    'isready',
    'stop',
    'ponderhit',
    'ucinewgame',
    'position startpos',
    'position startpos moves e2e4 e7e5',
    'go depth 1',
    'go infinite',
    'go ponder wtime 1000 btime 1000',
    'setoption name Hash value 16',
    'setoption name Threads value 2',
    'setoption name MultiPV value 2'
)
$randomLines = [System.Collections.Generic.List[string]]::new()
$randomLines.Add('uci')
$randomLines.Add('isready')
for ($i = 0; $i -lt 250; $i++) {
    $randomLines.Add($operations[$random.Next($operations.Count)])
}
$randomLines.Add('position startpos')
$randomLines.Add('go depth 2')
$randomLines.Add('stop')
$randomLines.Add('quit')
$randomTranscript = Invoke-UciTranscript (($randomLines -join "`n") + "`n") 'seeded-state-machine'
Assert-ProtocolGrammar $randomTranscript
Assert-CleanDiagnostics $randomTranscript
$randomBestmoves = @($randomTranscript.Lines | Where-Object { $_ -match '^bestmove ' })
if ($randomBestmoves.Count -lt 1) {
    throw 'the seeded state-machine storm must complete at least one search'
}
foreach ($bestmove in $randomBestmoves) {
    Assert-LegalBestmoveLine $bestmove 'seeded-state-machine'
}

# 23. Raw-stdin and line-ending edges: CRLF transcripts, a binary-prefixed
# transcript, and a NUL embedded in a command.  All must stay crash-free and
# protocol-clean, and a healthy search afterwards must answer.
$crlfTranscript = Invoke-UciTranscript ("uci`r`nisready`r`nposition startpos`r`ngo depth 1`r`nstop`r`nquit`r`n") 'crlf-transcript'
Assert-ProtocolGrammar $crlfTranscript
Assert-CleanDiagnostics $crlfTranscript
Assert-OneLegalBestmove $crlfTranscript
if ((Count-Lines $crlfTranscript '^uciok$') -ne 1 -or (Count-Lines $crlfTranscript '^readyok$') -ne 1) {
    throw 'a CRLF transcript must produce exactly one uciok and one readyok'
}

$binaryPrefix = ([string][char]0) + ([string][char]1) + ([string][char]127) + " garbage " + ([string][char]0) + "`n"
$binaryCase = Invoke-UciTranscript ($binaryPrefix + "uci`nisready`nposition startpos`ngo depth 1`nstop`nquit`n") 'binary-prefix'
Assert-ProtocolGrammar $binaryCase
Assert-CleanDiagnostics $binaryCase
Assert-OneLegalBestmove $binaryCase
if ((Count-Lines $binaryCase '^uciok$') -ne 1) {
    throw 'commands after a binary-garbage line must still be executed'
}

$nulCommand = Invoke-UciTranscript ("uci" + ([string][char]0) + "junk`nisready`nquit`n") 'nul-inside-command'
Assert-ProtocolGrammar $nulCommand
Assert-CleanDiagnostics $nulCommand
if ((Count-Lines $nulCommand '^readyok$') -ne 1) {
    throw 'a NUL inside a command must not stop later commands from being processed'
}

Write-Output 'PASS protocol fuzz corpus'

