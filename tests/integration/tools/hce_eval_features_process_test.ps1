param(
    [Parameter(Mandatory = $true)]
    [string]$ToolPath
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $ToolPath -PathType Leaf)) {
    throw "koi-eval-features executable is missing: $ToolPath"
}

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("koi-hce-eval-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch -Force | Out-Null
$inputPath = Join-Path $scratch 'fens.txt'
$outputPath = Join-Path $scratch 'evals.csv'
@(
    '4k3/8/8/4P3/8/8/8/4K3 w - - 0 1',
    'r1bq1rk1/ppp2ppp/2np1n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQ1RK1 w - - 0 1'
) | Set-Content -LiteralPath $inputPath -Encoding ASCII

function Invoke-Tool([string[]]$Arguments) {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $ToolPath
    $startInfo.Arguments = ($Arguments | ForEach-Object { "`"$_`"" }) -join ' '
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) { throw 'Unable to start koi-eval-features.' }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) {
        $process.Kill(); $process.WaitForExit()
        throw 'koi-eval-features did not exit within 30000 ms.'
    }
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        Stdout = $stdoutTask.GetAwaiter().GetResult()
        Stderr = $stderrTask.GetAwaiter().GetResult()
    }
}

Write-Output '[1/2] run koi-eval-features --hce CSV evaluation'
$result = Invoke-Tool @('--hce', '--input', $inputPath, '--output', $outputPath)
if ($result.ExitCode -ne 0) { throw "koi-eval-features --hce failed: $($result.Stderr)" }
if ($result.Stderr.Length -ne 0) { throw "koi-eval-features --hce wrote stderr: $($result.Stderr)" }
$rows = @(Get-Content -LiteralPath $outputPath)
if ($rows.Count -ne 3 -or $rows[0] -cne 'fen,label_cp,hce_cp') {
    throw "HCE eval CSV is malformed: $($rows -join ' | ')"
}
$scores = @($rows | Select-Object -Skip 1 | ForEach-Object { [int](($_ -split ',')[2]) })
if ($scores.Count -ne 2 -or ($scores | ForEach-Object { [Math]::Abs($_) -lt 99000 }).Contains($false)) {
    throw "HCE eval CSV scores are outside the expected range: $($scores -join ', ')"
}

Write-Output '[2/2] verify --hce and --nnue conflict rejection'
$conflict = Invoke-Tool @('--hce', '--nnue', 'missing.nnue')
if ($conflict.ExitCode -eq 0) {
    throw 'koi-eval-features must reject --hce together with --nnue.'
}
