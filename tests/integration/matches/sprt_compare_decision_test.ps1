$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$scriptPath = Join-Path $repositoryRoot 'tools/stability/sprt_compare.ps1'
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    $scriptPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) {
    throw "Could not parse sprt_compare.ps1: $($parseErrors -join '; ')"
}

$decisionFunction = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Get-CombinedSprtResult'
    }, $true)
if ($null -eq $decisionFunction) {
    throw 'sprt_compare.ps1 must expose its combined SPRT decision calculation as Get-CombinedSprtResult.'
}

. ([scriptblock]::Create($decisionFunction.Extent.Text))
$threshold = [Math]::Log(19.0)

function Assert-CombinedResult([double]$WhiteLlr, [double]$BlackLlr,
                               [string]$ExpectedDecision, [string]$Scenario) {
    $result = Get-CombinedSprtResult -WhiteLlr $WhiteLlr -BlackLlr $BlackLlr `
        -AcceptThreshold $threshold -RejectThreshold (-$threshold)
    $expectedLlr = $WhiteLlr + $BlackLlr
    if ($result.decision -ne $ExpectedDecision -or
        [Math]::Abs([double]$result.llr - $expectedLlr) -gt 0.000001) {
        throw "$Scenario expected combined LLR $expectedLlr and decision '$ExpectedDecision'; got LLR $($result.llr) and '$($result.decision)'."
    }
}

Write-Output '[1/5] verify Get-CombinedSprtResult aggregate LLR decisions'
# The white run accepts and the black run rejects independently, but their
# combined evidence is inconclusive. Neither per-color result may override it.
Assert-CombinedResult 3.1 -2.9 'inconclusive' 'opposed color decisions with inconclusive combined evidence'

# A white acceptance must not hide stronger combined evidence for rejection.
Assert-CombinedResult 3.2 -6.3 'reject' 'opposed color decisions with combined rejection'

# Positive combined evidence still accepts when the aggregate crosses its bound.
Assert-CombinedResult 1.6 1.5 'accept' 'combined acceptance'

Write-Output '[2/5] verify Start-ColorRun deterministic seed and disabled-book arguments'
# Capture the real per-color command construction without starting a match.
$startFunction = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Start-ColorRun'
    }, $true)
if ($null -eq $startFunction) { throw 'Missing Start-ColorRun function.' }
. ([scriptblock]::Create($startFunction.Extent.Text))
$root = Join-Path ([System.IO.Path]::GetTempPath()) 'koi-sprt-argument-test'
$matchScript = $scriptPath
$CandidatePath = $scriptPath
$BaselinePath = $scriptPath
$ReplayPath = $scriptPath
$OpeningFile = $scriptPath
$Threads = 1
$Hash = 64
$Games = 1
$Elo0 = 0
$Elo1 = 5
$MinGames = 1
$MaxGames = 1
$TimeControl = ''
$Nodes = 100
function Start-Process {
    param([string]$FilePath, [object[]]$ArgumentList, [switch]$PassThru,
          [string]$WindowStyle, [string]$RedirectStandardOutput,
          [string]$RedirectStandardError)
    return [pscustomobject]@{ arguments = @($ArgumentList) }
}
$invocation = Start-ColorRun 'white'
$arguments = @($invocation.arguments)
if (($arguments -join ' ') -notmatch '(?:^| )-KoiRandomSeed 0(?: |$)' -or
    ($arguments -join ' ') -notmatch '(?:^| )-OpponentOwnBook false(?: |$)') {
    throw 'Baseline comparison must use equal deterministic seed and disabled-book settings.'
}

Write-Output '[3/5] verify default Games=1 and Get-MoveSequenceHash'
# Default Games=1: repeated games per opening and color are exactly what the
# duplicate-game defect produced, so the default must not request them.
$gamesParameters = @($ast.ParamBlock.Parameters | Where-Object { $_.Name.VariablePath.UserPath -eq 'Games' })
if ($gamesParameters.Count -ne 1 -or $gamesParameters[0].DefaultValue.Extent.Text.Trim() -ne '1') {
    throw 'sprt_compare.ps1 must default to one game per opening and color.'
}

$hashFunction = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Get-MoveSequenceHash'
    }, $true)
if ($null -eq $hashFunction) {
    throw 'sprt_compare.ps1 must expose Get-MoveSequenceHash for duplicate-game detection.'
}
. ([scriptblock]::Create($hashFunction.Extent.Text))
$whiteHash = Get-MoveSequenceHash -KoiColor 'white' -Moves @('e2e4', 'e7e5')
$whiteHashAgain = Get-MoveSequenceHash -KoiColor 'white' -Moves @('e2e4', 'e7e5')
$blackHash = Get-MoveSequenceHash -KoiColor 'black' -Moves @('e2e4', 'e7e5')
$emptyHash = Get-MoveSequenceHash -KoiColor 'white' -Moves @()
if ($null -eq $whiteHash -or $whiteHash -ne $whiteHashAgain -or
    $whiteHash -eq $blackHash -or $null -ne $emptyHash) {
    throw 'The move-sequence hash must be deterministic, color-aware, and empty for move-less games.'
}

Write-Output '[4/5] verify Get-DuplicateGameAnalysis effective_games collapsing'
$analysisFunction = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Get-DuplicateGameAnalysis'
    }, $true)
if ($null -eq $analysisFunction) {
    throw 'sprt_compare.ps1 must expose Get-DuplicateGameAnalysis for duplicate-game detection.'
}
. ([scriptblock]::Create($analysisFunction.Extent.Text))
$duplicateRecords = @(
    [pscustomobject]@{ move_hash = 'aa' },
    [pscustomobject]@{ move_hash = 'aa' },
    [pscustomobject]@{ move_hash = 'bb' },
    [pscustomobject]@{ move_hash = $null }
)
$duplicateResult = Get-DuplicateGameAnalysis -Games $duplicateRecords
if ($duplicateResult.effective_games -ne 3 -or
    $duplicateResult.duplicate_games_detected -ne $true -or
    $duplicateResult.duplicate_games -ne 1 -or
    @($duplicateResult.duplicate_groups).Count -ne 1) {
    throw 'Duplicate games must collapse into effective_games while the repeated pair is reported.'
}
$distinctRecords = @(
    [pscustomobject]@{ move_hash = 'aa' },
    [pscustomobject]@{ move_hash = 'bb' }
)
$distinctResult = Get-DuplicateGameAnalysis -Games $distinctRecords
if ($distinctResult.effective_games -ne 2 -or
    $distinctResult.duplicate_games_detected -ne $false -or
    $distinctResult.duplicate_games -ne 0) {
    throw 'Distinct games must keep the full effective_games count without a duplicate warning.'
}

Write-Output '[5/5] verify EtaSupport.psm1 and Read-LatestSprtProgress estimators'
Import-Module (Join-Path $repositoryRoot 'tools/stability/EtaSupport.psm1') -Force
if ((Format-DurationSeconds 0) -ne '0.0s' -or
    (Format-DurationSeconds 12.4) -ne '12s' -or
    (Format-DurationSeconds 65) -ne '1m05s' -or
    (Format-DurationSeconds 3720) -ne '1h02m' -or
    (Format-DurationSeconds 90060) -ne '1d01h01m' -or
    (Format-DurationSeconds (-1)) -ne 'n/a' -or
    (Format-DurationSeconds ([double]::NaN)) -ne 'n/a') {
    throw 'Format-DurationSeconds produced unexpected formatting.'
}

$linWarm = Get-LinearEta -Completed 0 -Total 10 -ElapsedSeconds 5
$linDone = Get-LinearEta -Completed 10 -Total 10 -ElapsedSeconds 5
$linMid = Get-LinearEta -Completed 25 -Total 100 -ElapsedSeconds 10
if ($linWarm.eta_text -ne 'calculating' -or
    $linDone.eta_text -ne '0.0s' -or
    $linMid.eta_text -ne '~30s' -or
    [Math]::Abs([double]$linMid.remaining_seconds - 30.0) -gt 0.01) {
    throw "Get-LinearEta mismatch: $($linMid | ConvertTo-Json -Compress)"
}

$sprtZero = Get-SprtEta -Llr 0.0 -Lower (-2.944) -Upper 2.944 -Games 0 -MinGames 100 -MaxGames 1000 -ElapsedSeconds 10
$sprtWarm = Get-SprtEta -Llr 0.5 -Lower (-2.944) -Upper 2.944 -Games 5 -MinGames 100 -MaxGames 1000 -ElapsedSeconds 10
$sprtDone = Get-SprtEta -Llr 3.0 -Lower (-2.944) -Upper 2.944 -Games 100 -MinGames 100 -MaxGames 1000 -ElapsedSeconds 50 -Decision 'accept'
$sprtPos = Get-SprtEta -Llr 1.472 -Lower (-2.944) -Upper 2.944 -Games 100 -MinGames 50 -MaxGames 1000 -ElapsedSeconds 100
$sprtNeg = Get-SprtEta -Llr (-1.472) -Lower (-2.944) -Upper 2.944 -Games 100 -MinGames 50 -MaxGames 1000 -ElapsedSeconds 100
$sprtFloor = Get-SprtEta -Llr 2.0 -Lower (-2.944) -Upper 2.944 -Games 50 -MinGames 200 -MaxGames 1000 -ElapsedSeconds 50
$sprtCap = Get-SprtEta -Llr 0.05 -Lower (-2.944) -Upper 2.944 -Games 100 -MinGames 50 -MaxGames 200 -ElapsedSeconds 100
if ($sprtZero.eta_text -ne 'calculating' -or
    $null -ne $sprtWarm.expected_games_remaining -or
    $sprtWarm.eta_text -notlike '<=*' -or
    $sprtDone.eta_text -ne '0.0s (done)' -or
    $sprtPos.expected_games_remaining -ne 100 -or
    $sprtNeg.expected_games_remaining -ne 100 -or
    $sprtFloor.expected_games_remaining -ne 150 -or
    $null -ne $sprtCap.expected_games_remaining -or
    $sprtCap.eta_text -notlike '<=*') {
    throw 'Get-SprtEta failed boundary, floor, cap, or warmup contract.'
}

$preStep = Format-StepProgress -Step 1 -Total 4 -Label 'smoke' -StartedUtc ([DateTime]::UtcNow.AddSeconds(-4))
$postStep = Format-StepProgress -Step 4 -Total 4 -Label 'smoke' -StartedUtc ([DateTime]::UtcNow.AddSeconds(-4)) -StepCompleted
if ($preStep -notmatch '^\[1/4\] smoke \| elapsed .+ \| eta calculating$' -or
    $postStep -notmatch '^\[4/4\] smoke \| elapsed .+ \| eta 0\.0s$') {
    throw "Format-StepProgress mismatch: pre='$preStep' post='$postStep'"
}

$readProgressFunction = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Read-LatestSprtProgress'
    }, $true)
if ($null -eq $readProgressFunction) {
    throw 'sprt_compare.ps1 must expose Read-LatestSprtProgress for aggregate progress tracking.'
}
. ([scriptblock]::Create($readProgressFunction.Extent.Text))
$progressLog = Join-Path ([System.IO.Path]::GetTempPath()) ("koi-sprt-progress-$([guid]::NewGuid().ToString('N')).log")
try {
    @(
        '[SPRT] Game 12/200 | +4 =6 -2 | Elo: +28.9 | LLR: 0,145 [-2,944, 2,944] | Elapsed 12s | ETA calculating'
        '[SPRT] Game 25/200 | +10 =11 -4 | Elo: +41.7 | LLR: 0.412 [-2.944, 2.944] | Elapsed 25s | ETA ~2m20s (cap 2m55s)'
        'non-sprt footer line'
    ) | Set-Content -LiteralPath $progressLog -Encoding UTF8
    $parsedProgress = Read-LatestSprtProgress -LogPath $progressLog
    if ($null -eq $parsedProgress -or $parsedProgress.games -ne 25 -or
        $parsedProgress.wins -ne 10 -or $parsedProgress.draws -ne 11 -or $parsedProgress.losses -ne 4 -or
        [Math]::Abs([double]$parsedProgress.llr - 0.412) -gt 0.0001) {
        throw "Read-LatestSprtProgress failed to parse latest SPRT status: $($parsedProgress | ConvertTo-Json -Compress)"
    }
} finally {
    Remove-Item -LiteralPath $progressLog -Force -ErrorAction SilentlyContinue
}

Write-Output 'PASS combined SPRT decision uses aggregate evidence only'
Write-Output 'PASS duplicate-game detection and effective-games reporting are covered'
Write-Output 'PASS ETA estimators and SPRT log progress reader are covered'
