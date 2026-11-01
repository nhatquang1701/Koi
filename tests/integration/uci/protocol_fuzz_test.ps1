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
go wtime 1000 btime 1000 winc 2147483647 binc -1 movestogo 2147483647
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
setoption name SyzygyPath value :::
position
position fen
position startpos moves
position fen 8/8/8/4k3/8/4K3/8/8 w - -
setoption name BookFile value C:\Windows
setoption name BookFile value
setoption name BookFile value ""
setoption name hash value 16
setoption name HASH value 16
setoption name NoSuchOption value 1
setoption name Threads value 8
setoption name Hash value 128
setoption name MultiPV value 256
setoption name SyzygyProbeLimit value 7
setoption name SyzygyProbeDepth value 100
setoption name SyzygyInteriorDepth value 100
setoption name UCI_Chess960 value true
debug on
debug off
register later
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

    # Mutate the book while a search is in flight: the active search must still
    # complete cleanly, and the next searches must see the new file state.
    Send-UciCommand $bookChurnSession 'position startpos'
    Send-UciCommand $bookChurnSession 'go movetime 2000'
    Start-Sleep -Milliseconds 400
    [System.IO.File]::WriteAllBytes($bookPath, [byte[]]::new(8))
    Send-UciCommand $bookChurnSession 'stop'
    [void](Read-UciUntil $bookChurnSession { param($line) $line -match '^bestmove ' } 'book-churn active mutation' -ValidateUciOutput)
    Search-BookChurn $bookChurnSession 'book-churn truncated-after-active search'
    [System.IO.File]::WriteAllBytes($bookPath, $bookBytes)
    Search-BookChurn $bookChurnSession 'book-churn restored-after-active search'
    Remove-Item -LiteralPath $bookPath -Force
    Search-BookChurn $bookChurnSession 'book-churn removed search'
    Send-UciCommand $bookChurnSession 'quit'
    $bookChurnLines = Complete-UciSession $bookChurnSession $false
    $bookChurnBestmoves = @($bookChurnLines | Where-Object { $_ -match '^bestmove ' })
    if ($bookChurnBestmoves.Count -ne 7) {
        throw "book hot-swap must complete seven searches, got $($bookChurnBestmoves.Count) bestmoves"
    }
    $bookMoveLines = @($bookChurnLines | Where-Object { $_ -match '^info string book move e2e4 ' })
    if ($bookMoveLines.Count -lt 4) {
        throw "a valid auto-detected book must be used through transient corruption (including a mid-search mutation), got $($bookMoveLines.Count) book moves"
    }
    if (@($bookChurnLines | Where-Object { $_ -match '^info string book detected' }).Count -ne 1) {
        throw 'auto-detected book announcement must appear exactly once'
    }

    # 9h. Fail-closed asset boundaries.  Book files that are too small, not a
    # multiple of 16 bytes, or over the 16 MiB cap must never be used, and
    # invalid EvalFile forms must be rejected while the engine stays usable.
    $assetBoundary = Join-Path $assetRoot 'engine-boundary'
    New-Item -ItemType Directory -Path $assetBoundary -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetBoundary $executableName)
    $boundaryBook = Join-Path $assetBoundary 'book.bin'
    foreach ($boundarySize in @(0, 1, 15, 17, 16777217)) {
        if ($boundarySize -gt 1048576) {
            # Sparse file: only the size matters for the fail-closed check.
            $sparse = [System.IO.File]::Create($boundaryBook)
            $sparse.SetLength([int64]$boundarySize)
            $sparse.Dispose()
        } else {
            [System.IO.File]::WriteAllBytes($boundaryBook, [byte[]]::new($boundarySize))
        }
        $boundarySession = Start-UciSession -Executable (Join-Path $assetBoundary $executableName)
        Send-UciCommand $boundarySession 'uci'
        [void](Read-UciUntil $boundarySession { param($line) $line -eq 'uciok' } "boundary $boundarySize uciok" -ValidateUciOutput)
        Send-UciCommand $boundarySession "setoption name BookFile value $boundaryBook"
        Send-UciCommand $boundarySession 'isready'
        [void](Read-UciUntil $boundarySession { param($line) $line -eq 'readyok' } "boundary $boundarySize readyok" -ValidateUciOutput)
        Send-UciCommand $boundarySession 'position startpos'
        Send-UciCommand $boundarySession 'go depth 1'
        $boundaryBest = Read-UciUntil $boundarySession { param($line) $line -match '^bestmove ' } "boundary $boundarySize bestmove" -ValidateUciOutput
        Assert-LegalBestmoveLine $boundaryBest "book-size-$boundarySize"
        Send-UciCommand $boundarySession 'quit'
        $boundaryLines = Complete-UciSession $boundarySession $false
        if (@($boundaryLines | Where-Object { $_ -match '^info string book move ' }).Count -ne 0) {
            throw "an unusable $boundarySize-byte book must never produce a book move"
        }
    }

    $assetEval = Join-Path $assetRoot 'engine-eval-invalid'
    New-Item -ItemType Directory -Path $assetEval -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetEval $executableName)
    $emptyNet = Join-Path $assetEval 'empty.nnue'
    [System.IO.File]::WriteAllBytes($emptyNet, [byte[]]::new(0))
    $randomNet = Join-Path $assetEval 'random.nnue'
    $randomBytes = [byte[]]::new(64)
    [System.Random]::new(20261031).NextBytes($randomBytes)
    [System.IO.File]::WriteAllBytes($randomNet, $randomBytes)
    $netDirectory = Join-Path $assetEval 'net-directory'
    New-Item -ItemType Directory -Path $netDirectory -Force | Out-Null

    $evalSession = Start-UciSession -Executable (Join-Path $assetEval $executableName)
    Send-UciCommand $evalSession 'uci'
    [void](Read-UciUntil $evalSession { param($line) $line -eq 'uciok' } 'invalid EvalFile uciok' -ValidateUciOutput)
    Send-UciCommand $evalSession 'isready'
    [void](Read-UciUntil $evalSession { param($line) $line -eq 'readyok' } 'invalid EvalFile readyok' -ValidateUciOutput)
    foreach ($invalidNet in @($emptyNet, $netDirectory, $randomNet)) {
        Send-UciCommand $evalSession "setoption name EvalFile value $invalidNet"
        Send-UciCommand $evalSession 'isready'
        [void](Read-UciUntil $evalSession { param($line) $line -eq 'readyok' } 'invalid EvalFile readiness' -ValidateUciOutput)
        Send-UciCommand $evalSession 'position startpos'
        Send-UciCommand $evalSession 'go depth 1'
        $evalBest = Read-UciUntil $evalSession { param($line) $line -match '^bestmove ' } 'invalid EvalFile bestmove' -ValidateUciOutput
        Assert-LegalBestmoveLine $evalBest 'invalid-evalfile'
    }
    Send-UciCommand $evalSession 'quit'
    $evalLines = Complete-UciSession $evalSession $false
    $rejectionLines = @($evalLines | Where-Object { $_ -match '^info string EvalFile rejected: ' })
    if ($rejectionLines.Count -ne 3) {
        throw "every invalid EvalFile form must be rejected once, got $($rejectionLines.Count) rejection lines"
    }

    # 9i. Hot-add assets mid-session: a valid book created after boot is
    # detected and used on the next search; a corrupt Syzygy directory created
    # after boot is rejected silently and never crashes the search.
    $assetHotAdd = Join-Path $assetRoot 'engine-hot-add'
    New-Item -ItemType Directory -Path $assetHotAdd -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetHotAdd $executableName)
    $hotSession = Start-UciSession -Executable (Join-Path $assetHotAdd $executableName)
    Send-UciCommand $hotSession 'uci'
    [void](Read-UciUntil $hotSession { param($line) $line -eq 'uciok' } 'hot-add uciok' -ValidateUciOutput)
    Send-UciCommand $hotSession 'isready'
    [void](Read-UciUntil $hotSession { param($line) $line -eq 'readyok' } 'hot-add readyok' -ValidateUciOutput)

    Send-UciCommand $hotSession 'position startpos'
    Send-UciCommand $hotSession 'go depth 1'
    [void](Read-UciUntil $hotSession { param($line) $line -match '^bestmove ' } 'hot-add before book' -ValidateUciOutput)

    $hotBook = Join-Path $assetHotAdd 'book.bin'
    $hotBookBytes = [byte[]]::new(16)
    [byte[]]$hotKey = 0x46, 0x3b, 0x96, 0x18, 0x16, 0x91, 0xfc, 0x9c
    [Array]::Copy($hotKey, 0, $hotBookBytes, 0, 8)
    $hotBookBytes[8] = 0x03
    $hotBookBytes[9] = 0x1c
    $hotBookBytes[10] = 0x00
    $hotBookBytes[11] = 100
    [System.IO.File]::WriteAllBytes($hotBook, $hotBookBytes)

    Send-UciCommand $hotSession 'position startpos'
    Send-UciCommand $hotSession 'go depth 1'
    [void](Read-UciUntil $hotSession { param($line) $line -match '^bestmove ' } 'hot-add with book' -ValidateUciOutput)

    # CCRL: an explicit OwnBook=false must disable the auto-detected book even
    # though it was already detected and used once in this session.
    Send-UciCommand $hotSession 'setoption name OwnBook value false'
    Send-UciCommand $hotSession 'isready'
    [void](Read-UciUntil $hotSession { param($line) $line -eq 'readyok' } 'hot-add OwnBook=false readiness' -ValidateUciOutput)
    Send-UciCommand $hotSession 'position startpos'
    Send-UciCommand $hotSession 'go depth 1'
    [void](Read-UciUntil $hotSession { param($line) $line -match '^bestmove ' } 'hot-add OwnBook=false search' -ValidateUciOutput)

    $hotSyzygy = Join-Path $assetHotAdd 'syzygy'
    New-Item -ItemType Directory -Path $hotSyzygy -Force | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $hotSyzygy 'KQvK.rtbw'), [byte[]]::new(80))
    Send-UciCommand $hotSession 'isready'
    [void](Read-UciUntil $hotSession { param($line) $line -eq 'readyok' } 'hot-add syzygy readiness' -ValidateUciOutput)
    Send-UciCommand $hotSession 'position startpos'
    Send-UciCommand $hotSession 'go depth 1'
    [void](Read-UciUntil $hotSession { param($line) $line -match '^bestmove ' } 'hot-add with corrupt syzygy' -ValidateUciOutput)
    Send-UciCommand $hotSession 'quit'
    $hotLines = Complete-UciSession $hotSession $false
    if (@($hotLines | Where-Object { $_ -match '^info string book detected' }).Count -ne 1) {
        throw 'a book added mid-session must be detected exactly once'
    }
    if (@($hotLines | Where-Object { $_ -match '^info string book move e2e4 ' }).Count -ne 1) {
        throw 'a book added mid-session must be used exactly once before OwnBook=false disables it'
    }
    if (@($hotLines | Where-Object { $_ -match '^info string syzygy detected' }).Count -ne 0) {
        throw 'a corrupt Syzygy directory added mid-session must not be announced as usable'
    }

    # 9j. Transiently locked book (AV scanner / another process): the search
    # must fall back without a crash or a permanent negative cache, and the book
    # must be detected and used again once the lock is released.
    $assetLock = Join-Path $assetRoot 'engine-locked-book'
    New-Item -ItemType Directory -Path $assetLock -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetLock $executableName)
    $lockBook = Join-Path $assetLock 'book.bin'
    $lockBytes = [byte[]]::new(16)
    [byte[]]$lockKey = 0x46, 0x3b, 0x96, 0x18, 0x16, 0x91, 0xfc, 0x9c
    [Array]::Copy($lockKey, 0, $lockBytes, 0, 8)
    $lockBytes[8] = 0x03
    $lockBytes[9] = 0x1c
    $lockBytes[10] = 0x00
    $lockBytes[11] = 100
    [System.IO.File]::WriteAllBytes($lockBook, $lockBytes)

    $bookLock = $null
    $bookPermissionDenied = $false
    try {
        if ($IsWindows) {
            $bookLock = [System.IO.File]::Open($lockBook, [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
        } else {
            # On Unix, FileShare.None is not a mandatory cross-process lock, so
            # simulate the transient unavailability with chmod 000 instead.
            & chmod 000 $lockBook
            if ($LASTEXITCODE -ne 0) {
                throw 'failed to chmod the book to 000 for the transient-lock case'
            }
            $bookPermissionDenied = $true
        }
        $lockSession = Start-UciSession -Executable (Join-Path $assetLock $executableName)
        Send-UciCommand $lockSession 'uci'
        [void](Read-UciUntil $lockSession { param($line) $line -eq 'uciok' } 'locked-book uciok' -ValidateUciOutput)
        Send-UciCommand $lockSession 'isready'
        [void](Read-UciUntil $lockSession { param($line) $line -eq 'readyok' } 'locked-book readyok' -ValidateUciOutput)
        Send-UciCommand $lockSession 'position startpos'
        Send-UciCommand $lockSession 'go depth 1'
        [void](Read-UciUntil $lockSession { param($line) $line -match '^bestmove ' } 'locked-book first bestmove' -ValidateUciOutput)

        if ($IsWindows) {
            $bookLock.Dispose()
            $bookLock = $null
        } else {
            & chmod 644 $lockBook
            if ($LASTEXITCODE -ne 0) {
                throw 'failed to restore book permissions for the transient-lock case'
            }
            $bookPermissionDenied = $false
        }

        Send-UciCommand $lockSession 'isready'
        [void](Read-UciUntil $lockSession { param($line) $line -eq 'readyok' } 'released-book readyok' -ValidateUciOutput)
        Send-UciCommand $lockSession 'position startpos'
        Send-UciCommand $lockSession 'go depth 1'
        [void](Read-UciUntil $lockSession { param($line) $line -match '^bestmove ' } 'locked-book second bestmove' -ValidateUciOutput)
        Send-UciCommand $lockSession 'quit'
        $lockLines = Complete-UciSession $lockSession $false
    } finally {
        if ($null -ne $bookLock) {
            $bookLock.Dispose()
        }
        if ($bookPermissionDenied) {
            & chmod 644 $lockBook 2>$null
        }
    }
    if (@($lockLines | Where-Object { $_ -match '^bestmove ' }).Count -ne 2) {
        throw 'a transiently locked book must still complete both searches'
    }
    if (@($lockLines | Where-Object { $_ -match '^info string book detected' }).Count -ne 1) {
        throw 'a transiently locked book must be detected exactly once after the lock is released'
    }
    if (@($lockLines | Where-Object { $_ -match '^info string book move e2e4 ' }).Count -ne 1) {
        throw 'the released book must be used on the next search (no negative cache)'
    }

    # 9k. Debug logging with an unusable path: enabling Debug and pointing
    # DebugFile at a directory (then at a real file) must not disturb search or
    # stderr; diagnostics never crash the engine.
    $assetDebug = Join-Path $assetRoot 'engine-debug'
    New-Item -ItemType Directory -Path $assetDebug -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetDebug $executableName)
    $debugSession = Start-UciSession -Executable (Join-Path $assetDebug $executableName)
    Send-UciCommand $debugSession 'uci'
    [void](Read-UciUntil $debugSession { param($line) $line -eq 'uciok' } 'debug uciok' -ValidateUciOutput)
    Send-UciCommand $debugSession 'isready'
    [void](Read-UciUntil $debugSession { param($line) $line -eq 'readyok' } 'debug readyok' -ValidateUciOutput)
    Send-UciCommand $debugSession 'setoption name Debug value true'
    Send-UciCommand $debugSession "setoption name DebugFile value $assetDebug"
    Send-UciCommand $debugSession 'position startpos'
    Send-UciCommand $debugSession 'go depth 1'
    $debugBest = Read-UciUntil $debugSession { param($line) $line -match '^bestmove ' } 'debug directory bestmove' -ValidateUciOutput
    Assert-LegalBestmoveLine $debugBest 'debug-directory'
    Send-UciCommand $debugSession "setoption name DebugFile value $(Join-Path $assetDebug 'koi-debug.log')"
    Send-UciCommand $debugSession 'position startpos'
    Send-UciCommand $debugSession 'go depth 1'
    $debugBest2 = Read-UciUntil $debugSession { param($line) $line -match '^bestmove ' } 'debug file bestmove' -ValidateUciOutput
    Assert-LegalBestmoveLine $debugBest2 'debug-file'
    Send-UciCommand $debugSession 'quit'
    [void](Complete-UciSession $debugSession $false)

    # 9l. UCI_AnalyseMode bypasses the auto-detected book; turning it off later
    # in the same session restores detection and book use.
    $assetAnalyse = Join-Path $assetRoot 'engine-analyse-book'
    New-Item -ItemType Directory -Path $assetAnalyse -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetAnalyse $executableName)
    $analyseBook = Join-Path $assetAnalyse 'book.bin'
    $analyseBytes = [byte[]]::new(16)
    [byte[]]$analyseKey = 0x46, 0x3b, 0x96, 0x18, 0x16, 0x91, 0xfc, 0x9c
    [Array]::Copy($analyseKey, 0, $analyseBytes, 0, 8)
    $analyseBytes[8] = 0x03
    $analyseBytes[9] = 0x1c
    $analyseBytes[10] = 0x00
    $analyseBytes[11] = 100
    [System.IO.File]::WriteAllBytes($analyseBook, $analyseBytes)

    $analyseSession = Start-UciSession -Executable (Join-Path $assetAnalyse $executableName)
    Send-UciCommand $analyseSession 'uci'
    [void](Read-UciUntil $analyseSession { param($line) $line -eq 'uciok' } 'analyse-book uciok' -ValidateUciOutput)
    Send-UciCommand $analyseSession 'isready'
    [void](Read-UciUntil $analyseSession { param($line) $line -eq 'readyok' } 'analyse-book readyok' -ValidateUciOutput)
    Send-UciCommand $analyseSession 'setoption name UCI_AnalyseMode value true'
    Send-UciCommand $analyseSession 'isready'
    [void](Read-UciUntil $analyseSession { param($line) $line -eq 'readyok' } 'analyse-book analysis readiness' -ValidateUciOutput)
    Send-UciCommand $analyseSession 'position startpos'
    Send-UciCommand $analyseSession 'go depth 1'
    [void](Read-UciUntil $analyseSession { param($line) $line -match '^bestmove ' } 'analyse-book analysis bestmove' -ValidateUciOutput)
    $analysePhaseLineCount = $analyseSession.Lines.Count

    Send-UciCommand $analyseSession 'setoption name UCI_AnalyseMode value false'
    Send-UciCommand $analyseSession 'isready'
    [void](Read-UciUntil $analyseSession { param($line) $line -eq 'readyok' } 'analyse-book normal readiness' -ValidateUciOutput)
    Send-UciCommand $analyseSession 'position startpos'
    Send-UciCommand $analyseSession 'go depth 1'
    [void](Read-UciUntil $analyseSession { param($line) $line -match '^bestmove ' } 'analyse-book normal bestmove' -ValidateUciOutput)
    Send-UciCommand $analyseSession 'quit'
    $analyseLines = Complete-UciSession $analyseSession $false
    if (@($analyseLines | Where-Object { $_ -match '^bestmove ' }).Count -ne 2) {
        throw 'analysis-then-normal book case must complete two searches'
    }
    $analysisPhaseBookLines = @($analyseLines | Select-Object -First $analysePhaseLineCount |
        Where-Object { $_ -match '^info string book' })
    if ($analysisPhaseBookLines.Count -ne 0) {
        throw 'analysis mode must bypass the opening book'
    }
    $normalPhaseBookLines = @($analyseLines | Where-Object { $_ -match '^info string book' })
    if (@($normalPhaseBookLines | Where-Object { $_ -match '^info string book detected' }).Count -ne 1) {
        throw 'leaving analysis mode must auto-detect the book exactly once'
    }
    if (@($normalPhaseBookLines | Where-Object { $_ -match '^info string book move e2e4 ' }).Count -ne 1) {
        throw 'leaving analysis mode must use the book on the next search'
    }

    # 9m. Book eligibility matrix: MultiPV>1, infinite, ponder and searchmoves
    # all bypass the book; normal searches still use it (and MultiPV must return
    # to 1 for the book to come back).
    $assetElig = Join-Path $assetRoot 'engine-book-eligibility'
    New-Item -ItemType Directory -Path $assetElig -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetElig $executableName)
    $eligBook = Join-Path $assetElig 'book.bin'
    $eligBytes = [byte[]]::new(16)
    [byte[]]$eligKey = 0x46, 0x3b, 0x96, 0x18, 0x16, 0x91, 0xfc, 0x9c
    [Array]::Copy($eligKey, 0, $eligBytes, 0, 8)
    $eligBytes[8] = 0x03
    $eligBytes[9] = 0x1c
    $eligBytes[10] = 0x00
    $eligBytes[11] = 100
    [System.IO.File]::WriteAllBytes($eligBook, $eligBytes)

    $eligSession = Start-UciSession -Executable (Join-Path $assetElig $executableName)
    Send-UciCommand $eligSession 'uci'
    [void](Read-UciUntil $eligSession { param($line) $line -eq 'uciok' } 'eligibility uciok' -ValidateUciOutput)
    Send-UciCommand $eligSession 'isready'
    [void](Read-UciUntil $eligSession { param($line) $line -eq 'readyok' } 'eligibility readyok' -ValidateUciOutput)

    Send-UciCommand $eligSession 'setoption name MultiPV value 2'
    Send-UciCommand $eligSession 'position startpos'
    Send-UciCommand $eligSession 'go depth 1'
    [void](Read-UciUntil $eligSession { param($line) $line -match '^bestmove ' } 'eligibility multipv-2 bestmove' -ValidateUciOutput)
    $multipvPhase = @($eligSession.Lines | Where-Object { $_ -match '^info string book' })
    if ($multipvPhase.Count -ne 0) {
        throw 'MultiPV>1 must bypass the opening book'
    }

    Send-UciCommand $eligSession 'setoption name MultiPV value 1'
    Send-UciCommand $eligSession 'position startpos'
    Send-UciCommand $eligSession 'go depth 1'
    [void](Read-UciUntil $eligSession { param($line) $line -match '^bestmove ' } 'eligibility multipv-1 bestmove' -ValidateUciOutput)
    if (@($eligSession.Lines | Where-Object { $_ -match '^info string book move e2e4 ' }).Count -ne 1) {
        throw 'MultiPV=1 must use the opening book'
    }

    foreach ($bypass in @(
            @('go infinite', 'stop'),
            @('go ponder wtime 1000 btime 1000', 'stop'),
            @('go depth 1 searchmoves e2e4'))) {
        $before = $eligSession.Lines.Count
        Send-UciCommand $eligSession 'position startpos'
        Send-UciCommand $eligSession $bypass[0]
        if ($bypass.Count -gt 1) {
            Send-UciCommand $eligSession $bypass[1]
        }
        [void](Read-UciUntil $eligSession { param($line) $line -match '^bestmove ' } "eligibility bypass $($bypass[0]) bestmove" -ValidateUciOutput)
        $since = @($eligSession.Lines | Select-Object -Skip $before | Where-Object { $_ -match '^info string book' })
        if ($since.Count -ne 0) {
            throw "book bypass mode '$($bypass[0])' must not emit book lines"
        }
    }

    Send-UciCommand $eligSession 'position startpos'
    Send-UciCommand $eligSession 'go depth 1'
    [void](Read-UciUntil $eligSession { param($line) $line -match '^bestmove ' } 'eligibility restored bestmove' -ValidateUciOutput)
    Send-UciCommand $eligSession 'quit'
    $eligLines = Complete-UciSession $eligSession $false
    if (@($eligLines | Where-Object { $_ -match '^bestmove ' }).Count -ne 6) {
        throw 'the book eligibility matrix must complete six searches'
    }
    if (@($eligLines | Where-Object { $_ -match '^info string book detected' }).Count -ne 1) {
        throw 'the eligibility matrix must announce the book exactly once'
    }
    if (@($eligLines | Where-Object { $_ -match '^info string book move e2e4 ' }).Count -ne 2) {
        throw 'normal searches before and after the bypass matrix must both use the book'
    }

    # 9n. Explicit Syzygy path list with a corrupt component must fail closed;
    # an empty directory is not corrupt (it reports "no usable tables").
    $assetSyzygy = Join-Path $assetRoot 'engine-syzygy-list'
    New-Item -ItemType Directory -Path $assetSyzygy -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetSyzygy $executableName)
    $emptyTables = Join-Path $assetSyzygy 'empty-tables'
    $corruptTables = Join-Path $assetSyzygy 'corrupt-tables'
    New-Item -ItemType Directory -Path $emptyTables -Force | Out-Null
    New-Item -ItemType Directory -Path $corruptTables -Force | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $corruptTables 'KQvK.rtbw'), [byte[]]::new(80))
    $separator = [System.IO.Path]::PathSeparator
    $tablePathList = $emptyTables + $separator + $corruptTables

    $syzygySession = Start-UciSession -Executable (Join-Path $assetSyzygy $executableName)
    Send-UciCommand $syzygySession 'uci'
    [void](Read-UciUntil $syzygySession { param($line) $line -eq 'uciok' } 'syzygy-list uciok' -ValidateUciOutput)
    Send-UciCommand $syzygySession "setoption name SyzygyPath value $tablePathList"
    Send-UciCommand $syzygySession 'isready'
    [void](Read-UciUntil $syzygySession { param($line) $line -eq 'readyok' } 'syzygy-list readyok' -ValidateUciOutput)
    Send-UciCommand $syzygySession 'position startpos'
    Send-UciCommand $syzygySession 'go depth 1'
    $syzygyBest = Read-UciUntil $syzygySession { param($line) $line -match '^bestmove ' } 'syzygy-list bestmove' -ValidateUciOutput
    Assert-LegalBestmoveLine $syzygyBest 'syzygy-list'

    Send-UciCommand $syzygySession "setoption name SyzygyPath value $emptyTables"
    Send-UciCommand $syzygySession 'isready'
    [void](Read-UciUntil $syzygySession { param($line) $line -eq 'readyok' } 'syzygy-empty readyok' -ValidateUciOutput)
    Send-UciCommand $syzygySession 'position startpos'
    Send-UciCommand $syzygySession 'go depth 1'
    $syzygyEmptyBest = Read-UciUntil $syzygySession { param($line) $line -match '^bestmove ' } 'syzygy-empty bestmove' -ValidateUciOutput
    Assert-LegalBestmoveLine $syzygyEmptyBest 'syzygy-empty'
    Send-UciCommand $syzygySession 'quit'
    $syzygyLines = Complete-UciSession $syzygySession $false
    $corruptLines = @($syzygyLines | Where-Object {
        $_ -match '^info string Syzygy: corrupt tables at .+ \(probing disabled\)$'
    })
    if ($corruptLines.Count -ne 1) {
        throw 'a corrupt component in an explicit Syzygy path list must disable probing exactly once'
    }
    if (@($syzygyLines | Where-Object { $_ -match '^info string Syzygy: no usable tables at ' }).Count -lt 1) {
        throw 'an empty Syzygy directory must be reported as unusable, not corrupt'
    }

    # 9o. Unreadable book (ACL deny on Windows, chmod 000 on Unix): detection
    # must stay truthful (no "book detected") and the search must fall back;
    # replacing the file with a readable one is detected on the next search.
    $assetPerm = Join-Path $assetRoot 'engine-unreadable-book'
    New-Item -ItemType Directory -Path $assetPerm -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetPerm $executableName)
    $permBook = Join-Path $assetPerm 'book.bin'
    $permBytes = [byte[]]::new(16)
    [byte[]]$permKey = 0x46, 0x3b, 0x96, 0x18, 0x16, 0x91, 0xfc, 0x9c
    [Array]::Copy($permKey, 0, $permBytes, 0, 8)
    $permBytes[8] = 0x03
    $permBytes[9] = 0x1c
    $permBytes[10] = 0x00
    $permBytes[11] = 100
    [System.IO.File]::WriteAllBytes($permBook, $permBytes)
    $permissionDenied = $false
    try {
        if ($IsWindows) {
            & icacls $permBook /deny "${env:USERNAME}:R" | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw 'failed to apply the read-deny ACL to the book'
            }
        } else {
            & chmod 000 $permBook
            if ($LASTEXITCODE -ne 0) {
                throw 'failed to chmod the book to 000'
            }
        }
        $readable = $true
        try {
            $probeStream = [System.IO.File]::OpenRead($permBook)
            $probeStream.Dispose()
        } catch {
            $readable = $false
        }
        if ($readable) {
            throw 'the book was still readable after the permission change'
        }
        $permissionDenied = $true

        $permSession = Start-UciSession -Executable (Join-Path $assetPerm $executableName)
        Send-UciCommand $permSession 'uci'
        [void](Read-UciUntil $permSession { param($line) $line -eq 'uciok' } 'unreadable-book uciok' -ValidateUciOutput)
        Send-UciCommand $permSession 'isready'
        [void](Read-UciUntil $permSession { param($line) $line -eq 'readyok' } 'unreadable-book readyok' -ValidateUciOutput)
        Send-UciCommand $permSession 'position startpos'
        Send-UciCommand $permSession 'go depth 1'
        [void](Read-UciUntil $permSession { param($line) $line -match '^bestmove ' } 'unreadable-book bestmove' -ValidateUciOutput)
        $afterDenied = @($permSession.Lines | Where-Object { $_ -match '^info string book detected' })
        if ($afterDenied.Count -ne 0) {
            throw 'an unreadable book must not be announced as detected'
        }

        # Replace the unreadable file with a readable one (no negative cache).
        if ($IsWindows) {
            [System.IO.File]::Delete($permBook)
        } else {
            Remove-Item -LiteralPath $permBook -Force
        }
        [System.IO.File]::WriteAllBytes($permBook, $permBytes)
        if (-not $IsWindows) {
            & chmod 644 $permBook
        }
        $permissionDenied = $false

        Send-UciCommand $permSession 'isready'
        [void](Read-UciUntil $permSession { param($line) $line -eq 'readyok' } 'readable-book readyok' -ValidateUciOutput)
        Send-UciCommand $permSession 'position startpos'
        Send-UciCommand $permSession 'go depth 1'
        [void](Read-UciUntil $permSession { param($line) $line -match '^bestmove ' } 'readable-book bestmove' -ValidateUciOutput)
        Send-UciCommand $permSession 'quit'
        $permLines = Complete-UciSession $permSession $false
        if (@($permLines | Where-Object { $_ -match '^bestmove ' }).Count -ne 2) {
            throw 'the unreadable-book case must complete two searches'
        }
        if (@($permLines | Where-Object { $_ -match '^info string book detected' }).Count -ne 1) {
            throw 'a readable replacement book must be detected exactly once'
        }
        if (@($permLines | Where-Object { $_ -match '^info string book move e2e4 ' }).Count -ne 1) {
            throw 'the readable replacement book must be used on the next search'
        }
    } finally {
        if ($permissionDenied) {
            try {
                if ($IsWindows) {
                    [System.IO.File]::Delete($permBook)
                } else {
                    Remove-Item -LiteralPath $permBook -Force -ErrorAction SilentlyContinue
                }
            } catch {
            }
        }
    }

    # 9p. Unreadable Syzygy table file: reported as unreadable (not corrupt),
    # probing disabled, search legal.
    $assetSyzygyPerm = Join-Path $assetRoot 'engine-unreadable-syzygy'
    New-Item -ItemType Directory -Path $assetSyzygyPerm -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetSyzygyPerm $executableName)
    $syzygyPermDir = Join-Path $assetSyzygyPerm 'tables'
    New-Item -ItemType Directory -Path $syzygyPermDir -Force | Out-Null
    $syzygyPermTable = Join-Path $syzygyPermDir 'KQvK.rtbw'
    [byte[]]$syzygyTableBytes = [byte[]]::new(80)
    [byte[]]$syzygyMagic = 0x71, 0xE8, 0x23, 0x5D
    [Array]::Copy($syzygyMagic, 0, $syzygyTableBytes, 0, 4)
    [System.IO.File]::WriteAllBytes($syzygyPermTable, $syzygyTableBytes)
    $syzygyDenied = $false
    try {
        if ($IsWindows) {
            & icacls $syzygyPermTable /deny "${env:USERNAME}:R" | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw 'failed to apply the read-deny ACL to the Syzygy table'
            }
        } else {
            & chmod 000 $syzygyPermTable
            if ($LASTEXITCODE -ne 0) {
                throw 'failed to chmod the Syzygy table to 000'
            }
        }
        $readable = $true
        try {
            $probeStream = [System.IO.File]::OpenRead($syzygyPermTable)
            $probeStream.Dispose()
        } catch {
            $readable = $false
        }
        if ($readable) {
            throw 'the Syzygy table was still readable after the permission change'
        }
        $syzygyDenied = $true

        $syzygyPermSession = Start-UciSession -Executable (Join-Path $assetSyzygyPerm $executableName)
        Send-UciCommand $syzygyPermSession 'uci'
        [void](Read-UciUntil $syzygyPermSession { param($line) $line -eq 'uciok' } 'unreadable-syzygy uciok' -ValidateUciOutput)
        Send-UciCommand $syzygyPermSession "setoption name SyzygyPath value $syzygyPermDir"
        Send-UciCommand $syzygyPermSession 'isready'
        [void](Read-UciUntil $syzygyPermSession { param($line) $line -eq 'readyok' } 'unreadable-syzygy readyok' -ValidateUciOutput)
        Send-UciCommand $syzygyPermSession 'position startpos'
        Send-UciCommand $syzygyPermSession 'go depth 1'
        $syzygyPermBest = Read-UciUntil $syzygyPermSession { param($line) $line -match '^bestmove ' } 'unreadable-syzygy bestmove' -ValidateUciOutput
        Assert-LegalBestmoveLine $syzygyPermBest 'unreadable-syzygy'
        Send-UciCommand $syzygyPermSession 'quit'
        $syzygyPermLines = Complete-UciSession $syzygyPermSession $false
        $unreadableLines = @($syzygyPermLines | Where-Object {
            $_ -match '^info string Syzygy: unreadable tables at .+ \(probing disabled\)$'
        })
        if ($unreadableLines.Count -ne 1) {
            throw 'an unreadable Syzygy table must be reported as unreadable exactly once'
        }
        if (@($syzygyPermLines | Where-Object { $_ -match '^info string Syzygy: corrupt tables at ' }).Count -ne 0) {
            throw 'an unreadable Syzygy table must not be reported as corrupt'
        }
    } finally {
        if ($syzygyDenied) {
            try {
                if ($IsWindows) {
                    [System.IO.File]::Delete($syzygyPermTable)
                } else {
                    Remove-Item -LiteralPath $syzygyPermTable -Force -ErrorAction SilentlyContinue
                }
            } catch {
            }
        }
    }

    # 9q. EvalFile pointing at an unreadable network: rejected exactly once,
    # current evaluator preserved, next search legal.
    if ([string]::IsNullOrWhiteSpace($env:KOI_NNUE_PATH) -or
        -not (Test-Path -LiteralPath $env:KOI_NNUE_PATH -PathType Leaf)) {
        throw 'the fuzz test needs KOI_NNUE_PATH for the unreadable-EvalFile case'
    }
    $assetEval = Join-Path $assetRoot 'engine-unreadable-evalfile'
    New-Item -ItemType Directory -Path $assetEval -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetEval $executableName)
    $deniedNet = Join-Path $assetEval 'denied.nnue'
    Copy-Item -LiteralPath $env:KOI_NNUE_PATH -Destination $deniedNet
    $evalDenied = $false
    try {
        if ($IsWindows) {
            & icacls $deniedNet /deny "${env:USERNAME}:R" | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw 'failed to apply the read-deny ACL to the network'
            }
        } else {
            & chmod 000 $deniedNet
            if ($LASTEXITCODE -ne 0) {
                throw 'failed to chmod the network to 000'
            }
        }
        $readable = $true
        try {
            $probeStream = [System.IO.File]::OpenRead($deniedNet)
            $probeStream.Dispose()
        } catch {
            $readable = $false
        }
        if ($readable) {
            throw 'the network was still readable after the permission change'
        }
        $evalDenied = $true

        $evalSession = Start-UciSession -Executable (Join-Path $assetEval $executableName)
        Send-UciCommand $evalSession 'uci'
        [void](Read-UciUntil $evalSession { param($line) $line -eq 'uciok' } 'unreadable-eval uciok' -ValidateUciOutput)
        Send-UciCommand $evalSession "setoption name EvalFile value $deniedNet"
        Send-UciCommand $evalSession 'isready'
        [void](Read-UciUntil $evalSession { param($line) $line -eq 'readyok' } 'unreadable-eval readyok' -ValidateUciOutput)
        Send-UciCommand $evalSession 'position startpos'
        Send-UciCommand $evalSession 'go depth 1'
        $evalBest = Read-UciUntil $evalSession { param($line) $line -match '^bestmove ' } 'unreadable-eval bestmove' -ValidateUciOutput
        Assert-LegalBestmoveLine $evalBest 'unreadable-eval'
        Send-UciCommand $evalSession 'quit'
        $evalLines = Complete-UciSession $evalSession $false
        if (@($evalLines | Where-Object { $_ -match '^info string EvalFile rejected: ' }).Count -ne 1) {
            throw 'an unreadable EvalFile must be rejected exactly once'
        }
        if (@($evalLines | Where-Object { $_ -match '^bestmove ' }).Count -ne 1) {
            throw 'the search after an unreadable EvalFile must still produce one bestmove'
        }
    } finally {
        if ($evalDenied) {
            try {
                if ($IsWindows) {
                    [System.IO.File]::Delete($deniedNet)
                } else {
                    Remove-Item -LiteralPath $deniedNet -Force -ErrorAction SilentlyContinue
                }
            } catch {
            }
        }
    }

    # 9r. Linked book file: a hardlink on Windows, a symlink on Unix. The link
    # must resolve for detection/use; on Unix, breaking the link must fall back
    # silently without a crash.
    $assetLink = Join-Path $assetRoot 'engine-linked-book'
    New-Item -ItemType Directory -Path $assetLink -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetLink $executableName)
    $linkTarget = Join-Path $assetLink 'real-book.bin'
    $linkBook = Join-Path $assetLink 'book.bin'
    $linkBytes = [byte[]]::new(16)
    [byte[]]$linkKey = 0x46, 0x3b, 0x96, 0x18, 0x16, 0x91, 0xfc, 0x9c
    [Array]::Copy($linkKey, 0, $linkBytes, 0, 8)
    $linkBytes[8] = 0x03
    $linkBytes[9] = 0x1c
    $linkBytes[10] = 0x00
    $linkBytes[11] = 100
    [System.IO.File]::WriteAllBytes($linkTarget, $linkBytes)
    if ($IsWindows) {
        New-Item -ItemType HardLink -Path $linkBook -Target $linkTarget | Out-Null
    } else {
        & ln -s $linkTarget $linkBook
        if ($LASTEXITCODE -ne 0) {
            throw 'failed to create the symlinked book'
        }
    }
    $linkSession = Start-UciSession -Executable (Join-Path $assetLink $executableName)
    Send-UciCommand $linkSession 'uci'
    [void](Read-UciUntil $linkSession { param($line) $line -eq 'uciok' } 'linked-book uciok' -ValidateUciOutput)
    Send-UciCommand $linkSession 'isready'
    [void](Read-UciUntil $linkSession { param($line) $line -eq 'readyok' } 'linked-book readyok' -ValidateUciOutput)
    Send-UciCommand $linkSession 'position startpos'
    Send-UciCommand $linkSession 'go depth 1'
    [void](Read-UciUntil $linkSession { param($line) $line -match '^bestmove ' } 'linked-book bestmove' -ValidateUciOutput)
    if (-not $IsWindows) {
        Remove-Item -LiteralPath $linkTarget -Force
        Send-UciCommand $linkSession 'isready'
        [void](Read-UciUntil $linkSession { param($line) $line -eq 'readyok' } 'broken-link readyok' -ValidateUciOutput)
        Send-UciCommand $linkSession 'position startpos'
        Send-UciCommand $linkSession 'go depth 1'
        [void](Read-UciUntil $linkSession { param($line) $line -match '^bestmove ' } 'broken-link bestmove' -ValidateUciOutput)
    }
    Send-UciCommand $linkSession 'quit'
    $linkLines = Complete-UciSession $linkSession $false
    $expectedBestmoves = if ($IsWindows) { 1 } else { 2 }
    if (@($linkLines | Where-Object { $_ -match '^bestmove ' }).Count -ne $expectedBestmoves) {
        throw 'the linked-book case must complete the expected number of searches'
    }
    if (@($linkLines | Where-Object { $_ -match '^info string book detected' }).Count -ne 1) {
        throw 'a linked book must be detected exactly once'
    }
    if (@($linkLines | Where-Object { $_ -match '^info string book move e2e4 ' }).Count -ne 1) {
        throw 'a linked book must be used exactly once'
    }

    # 9s. Crafted book entry whose move is illegal for the keyed position
    # (startpos key -> a1a8). The book may be announced as a readable file, but
    # the engine must never emit the illegal book move; it must fall back to a
    # legal search answer with no protocol noise.
    $assetIllegal = Join-Path $assetRoot 'engine-illegal-book-move'
    New-Item -ItemType Directory -Path $assetIllegal -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetIllegal $executableName)
    [byte[]]$illegalBookBytes = 0x46, 0x3b, 0x96, 0x18, 0x16, 0x91, 0xfc, 0x9c,
        0x00, 0x38, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
    [System.IO.File]::WriteAllBytes((Join-Path $assetIllegal 'book.bin'), $illegalBookBytes)
    $illegalSession = Start-UciSession -Executable (Join-Path $assetIllegal $executableName)
    Send-UciCommand $illegalSession 'uci'
    [void](Read-UciUntil $illegalSession { param($line) $line -eq 'uciok' } 'illegal-book uciok' -ValidateUciOutput)
    Send-UciCommand $illegalSession 'isready'
    [void](Read-UciUntil $illegalSession { param($line) $line -eq 'readyok' } 'illegal-book readyok' -ValidateUciOutput)
    Send-UciCommand $illegalSession 'position startpos'
    Send-UciCommand $illegalSession 'go depth 1'
    $illegalBest = Read-UciUntil $illegalSession { param($line) $line -match '^bestmove ' } 'illegal-book bestmove' -ValidateUciOutput
    Assert-LegalBestmoveLine $illegalBest 'illegal-book'
    if ($illegalBest -match '^bestmove a1a8') {
        throw 'an illegal crafted book move must never be returned'
    }
    Send-UciCommand $illegalSession 'quit'
    $illegalLines = Complete-UciSession $illegalSession $false
    if (@($illegalLines | Where-Object { $_ -match '^info string book move ' }).Count -ne 0) {
        throw 'an illegal crafted book move must not be announced as a book move'
    }
    if (@($illegalLines | Where-Object { $_ -match '^bestmove ' }).Count -ne 1) {
        throw 'the illegal-book case must complete exactly one search'
    }

    # 9t. Truncated standard Syzygy table (KQvK.rtbw is canonically 272 bytes,
    # here only magic + zeros in 80 bytes): the size manifest must reject the
    # directory before Fathom maps it.  Without the gate this used to hang the
    # search with no bestmove.
    $assetTruncatedTb = Join-Path $assetRoot 'engine-truncated-syzygy'
    New-Item -ItemType Directory -Path $assetTruncatedTb -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetTruncatedTb $executableName)
    $truncatedTables = Join-Path $assetTruncatedTb 'tables'
    New-Item -ItemType Directory -Path $truncatedTables -Force | Out-Null
    [byte[]]$truncatedWdl = [byte[]]::new(80)
    [byte[]]$wdlMagic = 0x71, 0xE8, 0x23, 0x5D
    [Array]::Copy($wdlMagic, 0, $truncatedWdl, 0, 4)
    [System.IO.File]::WriteAllBytes((Join-Path $truncatedTables 'KQvK.rtbw'), $truncatedWdl)
    $truncatedSession = Start-UciSession -Executable (Join-Path $assetTruncatedTb $executableName)
    Send-UciCommand $truncatedSession 'uci'
    [void](Read-UciUntil $truncatedSession { param($line) $line -eq 'uciok' } 'truncated-syzygy uciok' -ValidateUciOutput)
    Send-UciCommand $truncatedSession "setoption name SyzygyPath value $truncatedTables"
    Send-UciCommand $truncatedSession 'isready'
    [void](Read-UciUntil $truncatedSession { param($line) $line -eq 'readyok' } 'truncated-syzygy readyok' -ValidateUciOutput)
    Send-UciCommand $truncatedSession 'position startpos'
    Send-UciCommand $truncatedSession 'go depth 1'
    $truncatedBest = Read-UciUntil $truncatedSession { param($line) $line -match '^bestmove ' } 'truncated-syzygy bestmove' -ValidateUciOutput
    Assert-LegalBestmoveLine $truncatedBest 'truncated-syzygy'
    Send-UciCommand $truncatedSession 'quit'
    $truncatedLines = Complete-UciSession $truncatedSession $false
    if (@($truncatedLines | Where-Object { $_ -match '^info string Syzygy: corrupt tables at .+ \(probing disabled\)$' }).Count -ne 1) {
        throw 'a truncated standard Syzygy table must be rejected as corrupt exactly once'
    }
    if (@($truncatedLines | Where-Object { $_ -match '^info string Syzygy: [0-9]+-man tables' }).Count -ne 0) {
        throw 'a truncated standard Syzygy table must not be announced as usable'
    }

    # 9u. Non-regular NNUE path (FIFO on Unix, named pipe on Windows): opening
    # it for reading could block; load_file must reject it by type, and the
    # session must keep searching.
    $assetSpecial = Join-Path $assetRoot 'engine-special-evalfile'
    New-Item -ItemType Directory -Path $assetSpecial -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetSpecial $executableName)
    if ($IsWindows) {
        $specialEval = '\\.\pipe\koi-fuzz-nnue-' + [guid]::NewGuid().ToString('N')
    } else {
        $specialEval = Join-Path $assetSpecial 'koi.nnue.fifo'
        & mkfifo $specialEval
        if ($LASTEXITCODE -ne 0) {
            throw 'failed to create the FIFO for the EvalFile test'
        }
    }
    try {
        $specialSession = Start-UciSession -Executable (Join-Path $assetSpecial $executableName)
        Send-UciCommand $specialSession 'uci'
        [void](Read-UciUntil $specialSession { param($line) $line -eq 'uciok' } 'special-eval uciok' -ValidateUciOutput)
        Send-UciCommand $specialSession "setoption name EvalFile value $specialEval"
        Send-UciCommand $specialSession 'isready'
        [void](Read-UciUntil $specialSession { param($line) $line -eq 'readyok' } 'special-eval readyok' -ValidateUciOutput)
        Send-UciCommand $specialSession 'position startpos'
        Send-UciCommand $specialSession 'go depth 1'
        $specialBest = Read-UciUntil $specialSession { param($line) $line -match '^bestmove ' } 'special-eval bestmove' -ValidateUciOutput
        Assert-LegalBestmoveLine $specialBest 'special-eval'
        Send-UciCommand $specialSession 'quit'
        $specialLines = Complete-UciSession $specialSession $false
        if (@($specialLines | Where-Object { $_ -match '^info string EvalFile rejected: ' }).Count -ne 1) {
            throw 'a non-regular EvalFile must be rejected exactly once'
        }
        if (@($specialLines | Where-Object { $_ -match '^bestmove ' }).Count -ne 1) {
            throw 'the session must keep searching after a non-regular EvalFile'
        }
    } finally {
        if (-not $IsWindows -and (Test-Path -LiteralPath $specialEval)) {
            Remove-Item -LiteralPath $specialEval -Force -ErrorAction SilentlyContinue
        }
    }

    # 9v. DebugFile pointing at a FIFO (Unix) or named pipe (Windows): opening
    # it for append could block; it must be ignored by type and the session must
    # stay responsive.
    $assetDebugPipe = Join-Path $assetRoot 'engine-special-debug'
    New-Item -ItemType Directory -Path $assetDebugPipe -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetDebugPipe $executableName)
    if ($IsWindows) {
        $specialDebug = '\\.\pipe\koi-fuzz-debug-' + [guid]::NewGuid().ToString('N')
    } else {
        $specialDebug = Join-Path $assetDebugPipe 'koi-debug.fifo'
        & mkfifo $specialDebug
        if ($LASTEXITCODE -ne 0) {
            throw 'failed to create the FIFO for the DebugFile test'
        }
    }
    try {
        $debugPipeSession = Start-UciSession -Executable (Join-Path $assetDebugPipe $executableName)
        Send-UciCommand $debugPipeSession 'uci'
        [void](Read-UciUntil $debugPipeSession { param($line) $line -eq 'uciok' } 'special-debug uciok' -ValidateUciOutput)
        Send-UciCommand $debugPipeSession 'setoption name Debug value true'
        Send-UciCommand $debugPipeSession "setoption name DebugFile value $specialDebug"
        Send-UciCommand $debugPipeSession 'isready'
        [void](Read-UciUntil $debugPipeSession { param($line) $line -eq 'readyok' } 'special-debug readyok' -ValidateUciOutput)
        Send-UciCommand $debugPipeSession 'position startpos'
        Send-UciCommand $debugPipeSession 'go depth 1'
        $debugPipeBest = Read-UciUntil $debugPipeSession { param($line) $line -match '^bestmove ' } 'special-debug bestmove' -ValidateUciOutput
        Assert-LegalBestmoveLine $debugPipeBest 'special-debug'
        Send-UciCommand $debugPipeSession 'quit'
        $debugPipeLines = Complete-UciSession $debugPipeSession $false
        if (@($debugPipeLines | Where-Object { $_ -match '^bestmove ' }).Count -ne 1) {
            throw 'the session must keep searching with a non-regular DebugFile'
        }
    } finally {
        if (-not $IsWindows -and (Test-Path -LiteralPath $specialDebug)) {
            Remove-Item -LiteralPath $specialDebug -Force -ErrorAction SilentlyContinue
        }
    }

    # 9w. Maximum accepted book size (16 MiB = 1,048,576 entries).  Every entry
    # has the startpos key and e2e4, so the loader must build a large record set
    # for one key, pick e2e4, and stay responsive.
    $assetMaxBook = Join-Path $assetRoot 'engine-max-book'
    New-Item -ItemType Directory -Path $assetMaxBook -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetMaxBook $executableName)
    $maxBookPath = Join-Path $assetMaxBook 'book.bin'
    [byte[]]$maxBookEntry = 0x46, 0x3b, 0x96, 0x18, 0x16, 0x91, 0xfc, 0x9c,
        0x03, 0x1c, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
    $maxBookEntries = 1048576
    $maxBookStream = [System.IO.File]::Open($maxBookPath, [System.IO.FileMode]::Create,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try {
        $chunk = [byte[]]::new($maxBookEntry.Length * 1024)
        for ($i = 0; $i -lt 1024; $i++) {
            [Array]::Copy($maxBookEntry, 0, $chunk, $i * $maxBookEntry.Length, $maxBookEntry.Length)
        }
        for ($i = 0; $i -lt ($maxBookEntries / 1024); $i++) {
            $maxBookStream.Write($chunk, 0, $chunk.Length)
        }
    } finally {
        $maxBookStream.Dispose()
    }
    if ((Get-Item -LiteralPath $maxBookPath).Length -ne (16 * 1024 * 1024)) {
        throw 'the 16 MiB book fixture has the wrong size'
    }
    $maxBookSession = Start-UciSession -Executable (Join-Path $assetMaxBook $executableName)
    Send-UciCommand $maxBookSession 'uci'
    [void](Read-UciUntil $maxBookSession { param($line) $line -eq 'uciok' } 'max-book uciok' -ValidateUciOutput)
    Send-UciCommand $maxBookSession 'isready'
    [void](Read-UciUntil $maxBookSession { param($line) $line -eq 'readyok' } 'max-book readyok' -ValidateUciOutput)
    Send-UciCommand $maxBookSession 'position startpos'
    Send-UciCommand $maxBookSession 'go depth 1'
    $maxBookBest = Read-UciUntil $maxBookSession { param($line) $line -match '^bestmove ' } 'max-book bestmove' -ValidateUciOutput
    Assert-LegalBestmoveLine $maxBookBest 'max-book'
    Send-UciCommand $maxBookSession 'quit'
    $maxBookLines = Complete-UciSession $maxBookSession $false
    if (@($maxBookLines | Where-Object { $_ -match '^info string book detected' }).Count -ne 1) {
        throw 'the 16 MiB book must be detected exactly once'
    }
    if (@($maxBookLines | Where-Object { $_ -match '^info string book move e2e4 ' }).Count -ne 1) {
        throw 'the 16 MiB all-e2e4 book must return the book move'
    }
    if ($maxBookBest -notmatch '^bestmove e2e4') {
        throw 'the 16 MiB book search must complete with bestmove e2e4'
    }

    # 9y. Polyglot entries with malformed move encodings (from == to, invalid
    # promotion bits) must never be used; the search falls back legally.
    $assetBadMoves = Join-Path $assetRoot 'engine-malformed-book-moves'
    New-Item -ItemType Directory -Path $assetBadMoves -Force | Out-Null
    Copy-Item -LiteralPath $EnginePath -Destination (Join-Path $assetBadMoves $executableName)
    [byte[]]$badMoveBook = 0x46, 0x3b, 0x96, 0x18, 0x16, 0x91, 0xfc, 0x9c,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x3c, 0x1f, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x3c, 0x1f, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x3c, 0x1f, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x3c, 0x1f, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x3c, 0x1f, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x3c, 0x1f, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x3c, 0x1f, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
    [System.IO.File]::WriteAllBytes((Join-Path $assetBadMoves 'book.bin'), $badMoveBook)
    $badMoveSession = Start-UciSession -Executable (Join-Path $assetBadMoves $executableName)
    Send-UciCommand $badMoveSession 'uci'
    [void](Read-UciUntil $badMoveSession { param($line) $line -eq 'uciok' } 'bad-move-book uciok' -ValidateUciOutput)
    Send-UciCommand $badMoveSession 'isready'
    [void](Read-UciUntil $badMoveSession { param($line) $line -eq 'readyok' } 'bad-move-book readyok' -ValidateUciOutput)
    Send-UciCommand $badMoveSession 'position startpos'
    Send-UciCommand $badMoveSession 'go depth 1'
    $badMoveBest = Read-UciUntil $badMoveSession { param($line) $line -match '^bestmove ' } 'bad-move-book bestmove' -ValidateUciOutput
    Assert-LegalBestmoveLine $badMoveBest 'bad-move-book'
    Send-UciCommand $badMoveSession 'quit'
    $badMoveLines = Complete-UciSession $badMoveSession $false
    if (@($badMoveLines | Where-Object { $_ -match '^info string book move ' }).Count -ne 0) {
        throw 'malformed Polyglot move encodings must never be announced as book moves'
    }
    if (@($badMoveLines | Where-Object { $_ -match '^bestmove ' }).Count -ne 1) {
        throw 'the malformed-book-move case must complete exactly one search'
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
    'setoption name MultiPV value 2',
    'setoption name Hash value 8',
    'setoption name Hash value 64',
    'setoption name MultiPV value 1',
    'setoption name EvalFile value /nonexistent/swap.nnue',
    'setoption name BookFile value /nonexistent/book.bin',
    'setoption name SyzygyPath value /nonexistent/tablebases',
    'setoption name Clear Hash',
    'setoption name UCI_AnalyseMode value true',
    'setoption name UCI_AnalyseMode value false'
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

# 25. Exhaustive invalid-value pass over every advertised option.  Spin
# options get non-numeric, below-minimum and (where safe) above-maximum values;
# checks get non-boolean values; strings get empty/quoted/missing paths; buttons
# are pressed.  The engine must stay responsive and then finish a legal search.
$optionSession = Start-UciSession -Executable $EnginePath
Send-UciCommand $optionSession 'uci'
[void](Read-UciUntil $optionSession { param($line) $line -eq 'uciok' } 'option-matrix uciok' -ValidateUciOutput)
$optionLines = @($optionSession.Lines | Where-Object { $_ -match '^option name .+ type ' })
if ($optionLines.Count -lt 20) {
    throw "expected the full option table in the handshake, got $($optionLines.Count) options"
}
foreach ($optionLine in $optionLines) {
    if ($optionLine -notmatch '^option name (.+?) type (spin|check|string|button)(?: .*)?$') {
        continue
    }
    $optionName = $Matches[1]
    $optionType = $Matches[2]
    switch ($optionType) {
        'spin' {
            $invalidValues = [System.Collections.Generic.List[string]]::new()
            $invalidValues.Add('not-a-number')
            $invalidValues.Add('-1')
            if ($optionLine -match ' min (-?[0-9]+)') {
                $invalidValues.Add(([int64]$Matches[1] - 1).ToString())
            }
            if ($optionLine -match ' max ([0-9]+)') {
                $maximum = [int64]$Matches[1]
                # Avoid the memory-heavy Hash upper bound and any "auto" max (0),
                # otherwise this pass could ask for 4 GiB or 256 threads.
                if ($maximum -gt 0 -and $maximum -le 64 -and $optionName -ne 'Hash') {
                    $invalidValues.Add(($maximum + 1).ToString())
                }
            }
            foreach ($invalidValue in $invalidValues) {
                Send-UciCommand $optionSession "setoption name $optionName value $invalidValue"
            }
        }
        'check' {
            foreach ($invalidValue in @('maybe', '2', '')) {
                Send-UciCommand $optionSession "setoption name $optionName value $invalidValue"
            }
        }
        'string' {
            foreach ($invalidValue in @('', '""', '"C:\\definitely missing\\asset.bin"')) {
                Send-UciCommand $optionSession "setoption name $optionName value $invalidValue"
            }
        }
        'button' {
            Send-UciCommand $optionSession "setoption name $optionName"
        }
    }
    Send-UciCommand $optionSession 'isready'
    [void](Read-UciUntil $optionSession { param($line) $line -eq 'readyok' } "option $optionName readiness" -ValidateUciOutput)
}
Send-UciCommand $optionSession 'position startpos'
Send-UciCommand $optionSession 'go depth 1'
$optionBest = Read-UciUntil $optionSession { param($line) $line -match '^bestmove ' } 'option-matrix bestmove' -ValidateUciOutput
Assert-LegalBestmoveLine $optionBest 'option-matrix'
Send-UciCommand $optionSession 'quit'
[void](Complete-UciSession $optionSession $false)

# 26. CCRL-style profile: Hash 256, one thread, internal book and pondering off,
# ucinewgame, then a clock-based search (wtime/btime/winc/binc/movestogo).  The
# engine must answer one legal bestmove and stay protocol-clean.
$ccrl = Invoke-UciTranscript "uci`nisready`nsetoption name Hash value 256`nsetoption name Threads value 1`nsetoption name OwnBook value false`nsetoption name Ponder value false`nucinewgame`nisready`nposition startpos`ngo wtime 1000 btime 1000 winc 10 binc 10 movestogo 20`nstop`nquit`n" 'ccrl-profile'
Assert-ProtocolGrammar $ccrl
Assert-CleanDiagnostics $ccrl
Assert-OneLegalBestmove $ccrl

# 27. Raw control bytes and whitespace variants: NUL/control bytes inside
# option values, tab-separated commands, and ponder combined with searchmoves.
$nulBook = "uci`nisready`nsetoption name BookFile value C:\\koi" + ([string][char]0) + "book.bin`nposition startpos`ngo depth 1`nstop`nquit`n"
$nulBookCase = Invoke-UciTranscript $nulBook 'nul-in-bookfile'
Assert-ProtocolGrammar $nulBookCase
Assert-CleanDiagnostics $nulBookCase
Assert-OneLegalBestmove $nulBookCase

$nulEval = "uci`nisready`nsetoption name EvalFile value C:\\koi" + ([string][char]0) + "net.nnue`nposition startpos`ngo depth 1`nstop`nquit`n"
$nulEvalCase = Invoke-UciTranscript $nulEval 'nul-in-evalfile'
Assert-ProtocolGrammar $nulEvalCase
Assert-CleanDiagnostics $nulEvalCase
Assert-OneLegalBestmove $nulEvalCase
if ((Count-Lines $nulEvalCase '^info string EvalFile rejected: ') -ne 1) {
    throw 'a NUL-containing EvalFile must be rejected exactly once'
}

$controlValue = "a" + ([string][char]1) + ([string][char]2) + ([string][char]127) + "b"
$controlCase = Invoke-UciTranscript ("uci`nisready`nsetoption name BookFile value " + $controlValue + "`nposition startpos`ngo depth 1`nstop`nquit`n") 'control-bytes-option'
Assert-ProtocolGrammar $controlCase
Assert-CleanDiagnostics $controlCase
Assert-OneLegalBestmove $controlCase

$tabbed = Invoke-UciTranscript "uci`nisready`nposition`tfen`t8/8/8/4k3/8/4K3/8/8`tw`t-`t-`t0`t1`ngo`tdepth`t1`nstop`nquit`n" 'tab-separated-commands'
Assert-ProtocolGrammar $tabbed
Assert-CleanDiagnostics $tabbed
Assert-OneLegalBestmove $tabbed

$ponderSearchmoves = Invoke-UciTranscript "uci`nisready`nposition startpos`ngo ponder wtime 1000 btime 1000 searchmoves e2e4`nisready`nponderhit`nstop`nquit`n" 'ponder-searchmoves'
Assert-ProtocolGrammar $ponderSearchmoves
Assert-CleanDiagnostics $ponderSearchmoves
Assert-OnlyBestmove $ponderSearchmoves 'bestmove e2e4'

# 28. Ponder stop/ponderhit races: stopping a just-started ponder search,
# duplicate stops, duplicate ponderhit and ponderhit-after-stop must each leave
# exactly one legal bestmove and no protocol noise.
$ponderImmediate = Invoke-UciTranscript "uci`nisready`nposition startpos`ngo ponder wtime 1000 btime 1000`nstop`nquit`n" 'ponder-immediate-stop'
Assert-ProtocolGrammar $ponderImmediate
Assert-CleanDiagnostics $ponderImmediate
Assert-OneLegalBestmove $ponderImmediate

$ponderStopTwice = Invoke-UciTranscript "uci`nisready`nposition startpos`ngo ponder wtime 1000 btime 1000`nstop`nstop`nquit`n" 'ponder-stop-twice'
Assert-ProtocolGrammar $ponderStopTwice
Assert-CleanDiagnostics $ponderStopTwice
Assert-OneLegalBestmove $ponderStopTwice

$ponderhitTwice = Invoke-UciTranscript "uci`nisready`nposition startpos`ngo ponder wtime 1000 btime 1000`nponderhit`nponderhit`nisready`nstop`nquit`n" 'ponderhit-twice'
Assert-ProtocolGrammar $ponderhitTwice
Assert-CleanDiagnostics $ponderhitTwice
Assert-OneLegalBestmove $ponderhitTwice

$ponderhitAfterStop = Invoke-UciTranscript "uci`nisready`nposition startpos`ngo ponder wtime 1000 btime 1000`nstop`nponderhit`nisready`nquit`n" 'ponderhit-after-stop'
Assert-ProtocolGrammar $ponderhitAfterStop
Assert-CleanDiagnostics $ponderhitAfterStop
Assert-OneLegalBestmove $ponderhitAfterStop

$stopBeforeGo = Invoke-UciTranscript "uci`nisready`nposition startpos`nstop`nstop`ngo depth 1`nstop`nquit`n" 'stop-before-go'
Assert-ProtocolGrammar $stopBeforeGo
Assert-CleanDiagnostics $stopBeforeGo
Assert-OneLegalBestmove $stopBeforeGo

# 29. Ready-flood during an infinite search: 100 isready commands must each be
# answered while the search runs, with exactly one bestmove after stop.
$readyFlood = Invoke-UciTranscript ("uci`nisready`nposition startpos`ngo infinite`n" + ("isready`n" * 100) + "stop`nquit`n") 'isready-flood'
Assert-ProtocolGrammar $readyFlood
Assert-CleanDiagnostics $readyFlood
Assert-OneLegalBestmove $readyFlood
if ((Count-Lines $readyFlood '^readyok$') -ne 101) {
    throw '100 isready commands during a search must produce 101 readyok answers'
}

# 30. Very long asset paths (100 KB values, far beyond normal filesystem limits)
# must stay inert: the option is accepted/rejected safely and the next search is
# clean.
$longPathValue = 'a' * 100000
$longSeparatorValue = ';' * 100000
$longPathCase = Invoke-UciTranscript ("uci`nisready`nsetoption name BookFile value " +
    $longPathValue + "`nsetoption name EvalFile value " + $longPathValue +
    "`nsetoption name DebugFile value " + $longPathValue +
    "`nsetoption name SyzygyPath value " + $longSeparatorValue +
    "`nisready`nposition startpos`ngo depth 1`nstop`nquit`n") 'long-asset-paths'
Assert-ProtocolGrammar $longPathCase
Assert-CleanDiagnostics $longPathCase
Assert-OneLegalBestmove $longPathCase

Write-Output 'PASS protocol fuzz corpus'

