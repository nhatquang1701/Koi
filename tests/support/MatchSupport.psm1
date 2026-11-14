# MatchSupport.psm1
#
# Shared helpers for the scripted UCI match integration tests.  The match
# scripts import this module instead of re-declaring the PowerShell discovery
# and fixture-engine plumbing:
#
#   Import-Module ([System.IO.Path]::GetFullPath(
#       (Join-Path $PSScriptRoot '../../support/MatchSupport.psm1'))) -Force

function Get-PowerShellExecutable {
    <#
    .SYNOPSIS
        Return the path of the PowerShell host running the current script.
    #>
    if ($PSVersionTable.PSEdition -eq 'Core') {
        return (Get-Process -Id $PID -ErrorAction Stop).Path
    }

    $pwsh = Get-Command pwsh -ErrorAction SilentlyContinue
    if ($null -ne $pwsh) {
        return $pwsh.Source
    }

    return (Get-Command powershell.exe -ErrorAction Stop).Source
}

function New-ScriptedUciEngine {
    <#
    .SYNOPSIS
        Copy the scripted UCI fixture next to a match and name it.
    .DESCRIPTION
        Places $FixturePath in $Directory as "$Name.exe" on Windows and
        "$Name" elsewhere, then returns the engine descriptor ({ path, log })
        consumed by the match invocations.
    #>
    param(
        [Parameter(Mandatory = $true, Position = 0)]
        [string]$FixturePath,
        [Parameter(Mandatory = $true, Position = 1)]
        [string]$Directory,
        [Parameter(Mandatory = $true, Position = 2)]
        [string]$Name
    )

    $binarySuffix = if ($env:OS -eq 'Windows_NT') { '.exe' } else { '' }
    $path = Join-Path $Directory "$Name$binarySuffix"
    Copy-Item -LiteralPath $FixturePath -Destination $path
    return [pscustomobject]@{
        path = $path
        log = Join-Path $Directory "$Name.log"
    }
}

Import-Module ([System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '../../tools/stability/EtaSupport.psm1'))) -Force

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

Export-ModuleMember -Function Get-PowerShellExecutable, New-ScriptedUciEngine, Write-TestProgress
