# EtaSupport.psm1
#
# Shared duration formatting, linear ETA, and SPRT stopping-time estimation for
# Koi's long-running PowerShell test harnesses and match runners.

$script:InvariantCulture = [System.Globalization.CultureInfo]::InvariantCulture

function Format-DurationSeconds {
    <#
    .SYNOPSIS
        Format a non-negative duration in seconds into a concise string.
    #>
    param(
        [Parameter(Mandatory = $true)]
        [double]$Seconds
    )

    if ([double]::IsNaN($Seconds) -or [double]::IsInfinity($Seconds) -or $Seconds -lt 0.0) {
        return 'n/a'
    }
    if ($Seconds -lt 10.0) {
        return [string]::Format($script:InvariantCulture, '{0:0.0}s', $Seconds)
    }

    $rounded = [int64][Math]::Round($Seconds)
    if ($rounded -lt 60) {
        return [string]::Format($script:InvariantCulture, '{0}s', $rounded)
    }
    if ($rounded -lt 3600) {
        $minutes = [int64][Math]::Floor($rounded / 60)
        $remSeconds = $rounded % 60
        return [string]::Format($script:InvariantCulture, '{0}m{1:D2}s', $minutes, $remSeconds)
    }
    if ($rounded -lt 86400) {
        $hours = [int64][Math]::Floor($rounded / 3600)
        $minutes = [int64][Math]::Floor(($rounded % 3600) / 60)
        return [string]::Format($script:InvariantCulture, '{0}h{1:D2}m', $hours, $minutes)
    }

    $days = [int64][Math]::Floor($rounded / 86400)
    $remHours = [int64][Math]::Floor(($rounded % 86400) / 3600)
    $remMinutes = [int64][Math]::Floor(($rounded % 3600) / 60)
    return [string]::Format($script:InvariantCulture, '{0}d{1:D2}h{2:D2}m', $days, $remHours, $remMinutes)
}

function Get-LinearEta {
    <#
    .SYNOPSIS
        Estimate remaining wall time from completed work units and elapsed time.
    #>
    param(
        [Parameter(Mandatory = $true)]
        [double]$Completed,
        [Parameter(Mandatory = $true)]
        [double]$Total,
        [Parameter(Mandatory = $true)]
        [double]$ElapsedSeconds
    )

    $clampedElapsed = [Math]::Max(0.0, $ElapsedSeconds)
    $elapsedText = Format-DurationSeconds $clampedElapsed
    $ratePerHour = if ($Completed -gt 0.0 -and $clampedElapsed -gt 0.0) {
        ($Completed / $clampedElapsed) * 3600.0
    } else {
        $null
    }

    if ($Total -le 0.0) {
        return [pscustomobject]@{
            completed = $Completed
            total = $Total
            elapsed_seconds = $clampedElapsed
            elapsed_text = $elapsedText
            remaining_seconds = $null
            eta_text = 'n/a'
            rate_per_hour = $ratePerHour
        }
    }
    if ($Completed -ge $Total) {
        return [pscustomobject]@{
            completed = $Completed
            total = $Total
            elapsed_seconds = $clampedElapsed
            elapsed_text = $elapsedText
            remaining_seconds = 0.0
            eta_text = '0.0s'
            rate_per_hour = $ratePerHour
        }
    }
    if ($Completed -le 0.0 -or $clampedElapsed -le 0.0) {
        return [pscustomobject]@{
            completed = $Completed
            total = $Total
            elapsed_seconds = $clampedElapsed
            elapsed_text = $elapsedText
            remaining_seconds = $null
            eta_text = 'calculating'
            rate_per_hour = $ratePerHour
        }
    }

    $remainingSeconds = $clampedElapsed * (($Total - $Completed) / $Completed)
    return [pscustomobject]@{
        completed = $Completed
        total = $Total
        elapsed_seconds = $clampedElapsed
        elapsed_text = $elapsedText
        remaining_seconds = $remainingSeconds
        eta_text = ('~' + (Format-DurationSeconds $remainingSeconds))
        rate_per_hour = $ratePerHour
    }
}

function Get-SprtEta {
    <#
    .SYNOPSIS
        Estimate expected and worst-case SPRT remaining time from current LLR drift.
    #>
    param(
        [Parameter(Mandatory = $true)]
        [double]$Llr,
        [Parameter(Mandatory = $true)]
        [double]$Lower,
        [Parameter(Mandatory = $true)]
        [double]$Upper,
        [Parameter(Mandatory = $true)]
        [int]$Games,
        [Parameter(Mandatory = $true)]
        [int]$MinGames,
        [Parameter(Mandatory = $true)]
        [int]$MaxGames,
        [Parameter(Mandatory = $true)]
        [double]$ElapsedSeconds,
        [string]$Decision = 'running'
    )

    $clampedElapsed = [Math]::Max(0.0, $ElapsedSeconds)
    $elapsedText = Format-DurationSeconds $clampedElapsed
    $remMaxGames = [Math]::Max(0, $MaxGames - $Games)
    $gamesPerHour = if ($Games -gt 0 -and $clampedElapsed -gt 0.0) {
        ($Games / $clampedElapsed) * 3600.0
    } else {
        $null
    }

    if (($Decision -ne 'running' -and $Games -ge $MinGames) -or $Games -ge $MaxGames) {
        return [pscustomobject]@{
            elapsed_seconds = $clampedElapsed
            elapsed_text = $elapsedText
            expected_eta_seconds = 0.0
            max_eta_seconds = 0.0
            expected_games_remaining = 0
            max_games_remaining = 0
            games_per_hour = $gamesPerHour
            eta_text = '0.0s (done)'
        }
    }
    if ($Games -lt 1 -or $clampedElapsed -le 0.0) {
        return [pscustomobject]@{
            elapsed_seconds = $clampedElapsed
            elapsed_text = $elapsedText
            expected_eta_seconds = $null
            max_eta_seconds = $null
            expected_games_remaining = $null
            max_games_remaining = $remMaxGames
            games_per_hour = $gamesPerHour
            eta_text = 'calculating'
        }
    }

    $secPerGame = $clampedElapsed / [double]$Games
    $maxEtaSeconds = [double]$remMaxGames * $secPerGame
    $maxEtaText = Format-DurationSeconds $maxEtaSeconds
    $warmupGames = [Math]::Max(2, [Math]::Min(10, [Math]::Max(2, $MinGames)))

    $expGames = $null
    if ($Games -ge $warmupGames -and [Math]::Abs($Llr) -gt 1e-6) {
        $drift = [Math]::Abs($Llr) / [double]$Games
        $remLlr = if ($Llr -gt 0.0) {
            [Math]::Max(0.0, $Upper - $Llr)
        } else {
            [Math]::Max(0.0, $Llr - $Lower)
        }
        $minRemGames = [double][Math]::Max(0, $MinGames - $Games)
        $expGames = [Math]::Max($minRemGames, $remLlr / $drift)
    }

    if ($null -ne $expGames -and $expGames -lt [double]$remMaxGames) {
        $expEtaSeconds = $expGames * $secPerGame
        return [pscustomobject]@{
            elapsed_seconds = $clampedElapsed
            elapsed_text = $elapsedText
            expected_eta_seconds = $expEtaSeconds
            max_eta_seconds = $maxEtaSeconds
            expected_games_remaining = [int][Math]::Ceiling([Math]::Max(0.0, $expGames - 1e-9))
            max_games_remaining = $remMaxGames
            games_per_hour = $gamesPerHour
            eta_text = ('~' + (Format-DurationSeconds $expEtaSeconds) + " (cap $maxEtaText)")
        }
    }

    return [pscustomobject]@{
        elapsed_seconds = $clampedElapsed
        elapsed_text = $elapsedText
        expected_eta_seconds = $null
        max_eta_seconds = $maxEtaSeconds
        expected_games_remaining = $null
        max_games_remaining = $remMaxGames
        games_per_hour = $gamesPerHour
        eta_text = "<=$maxEtaText (cap)"
    }
}

function Format-StepProgress {
    <#
    .SYNOPSIS
        Format a step progress message with elapsed time and linear ETA.
    #>
    param(
        [Parameter(Mandatory = $true)]
        [int]$Step,
        [Parameter(Mandatory = $true)]
        [int]$Total,
        [Parameter(Mandatory = $true)]
        [string]$Label,
        [Parameter(Mandatory = $true)]
        [datetime]$StartedUtc,
        [switch]$StepCompleted
    )

    $completed = if ($StepCompleted) { $Step } else { [Math]::Max(0, $Step - 1) }
    $elapsedSeconds = ([DateTime]::UtcNow - $StartedUtc).TotalSeconds
    $eta = Get-LinearEta -Completed $completed -Total $Total -ElapsedSeconds $elapsedSeconds
    return "[$Step/$Total] $Label | elapsed $($eta.elapsed_text) | eta $($eta.eta_text)"
}

Export-ModuleMember -Function Format-DurationSeconds, Get-LinearEta, Get-SprtEta, Format-StepProgress
