param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath
)

$ErrorActionPreference = 'Stop'
Import-Module ([System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../support/UciSession.psm1'))) -Force

trap {
    Stop-AllUciSessions
    break
}

$previousEvaluator = $env:KOI_EVALUATOR
$previousAutodetect = $env:KOI_NNUE_AUTODETECT
try {
    # Neither a network nor the HCE's old fallback is needed: the explicit
    # development override must boot the clean evaluator and search legally.
    $env:KOI_EVALUATOR = 'hce'
    $env:KOI_NNUE_AUTODETECT = '0'

    $session = Start-UciSession -Executable $EnginePath
    try {
        Write-TestProgress 1 2 'HCE handshake and readiness'
        Send-UciCommand $session 'uci'
        $uciok = Read-UciUntil -Session $session -Description 'uciok' -Predicate { param($line) $line -eq 'uciok' }
        if ($uciok -ne 'uciok') {
            throw 'HCE engine did not complete the UCI handshake.'
        }
        Send-UciCommand $session 'isready'
        $readyok = Read-UciUntil -Session $session -Description 'readyok' -Predicate { param($line) $line -eq 'readyok' }
        if ($readyok -ne 'readyok') {
            throw 'HCE engine did not answer isready.'
        }

        Write-TestProgress 2 2 'HCE depth-4 search and clean shutdown'
        Send-UciCommand $session 'position startpos'
        Send-UciCommand $session 'go depth 4'
        $bestmove = Read-UciUntil -Session $session -Description 'bestmove' -Predicate {
            param($line) $line -like 'bestmove *'
        }
        if ($bestmove -notmatch '^bestmove [a-h][1-8][a-h][1-8][qrbn]?( ponder [a-h][1-8][a-h][1-8][qrbn]?)?$') {
            throw "HCE engine returned an illegal or missing bestmove: '$bestmove'."
        }
        $transcript = Complete-UciSession -Session $session -SendQuit $true
        if ($transcript -notcontains $uciok) {
            throw 'HCE transcript lost the UCI handshake.'
        }
    } finally {
        Stop-UciSession $session
    }
} finally {
    if ($null -eq $previousEvaluator) {
        Remove-Item Env:\KOI_EVALUATOR -ErrorAction SilentlyContinue
    } else {
        $env:KOI_EVALUATOR = $previousEvaluator
    }
    if ($null -eq $previousAutodetect) {
        Remove-Item Env:\KOI_NNUE_AUTODETECT -ErrorAction SilentlyContinue
    } else {
        $env:KOI_NNUE_AUTODETECT = $previousAutodetect
    }
}
