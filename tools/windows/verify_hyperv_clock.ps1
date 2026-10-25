# Installed-Windows probe. Clock stepping is opt-in and intended for a private VM.
[CmdletBinding()]
param(
    [string]$OutputPath = 'C:\PCEMU\HYPERV-CLOCK.JSON',
    [switch]$AllowClockStep
)
$ErrorActionPreference = 'Stop'
if (Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Control\MiniNT') { throw 'Installed Windows is required.' }
$ntpPath = 'HKLM:\SYSTEM\CurrentControlSet\Services\W32Time\TimeProviders\NtpClient'
$vmicPath = 'HKLM:\SYSTEM\CurrentControlSet\Services\W32Time\TimeProviders\VMICTimeProvider'
function Read-ClockSample {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $response = Invoke-WebRequest -Uri 'http://10.0.2.2:8080/probe' -UseBasicParsing -TimeoutSec 5
    $content = $response.Content
    if ($content -is [byte[]]) { $content = [Text.Encoding]::UTF8.GetString($content) }
    if (([string]$content).Trim() -ne 'PCEMU_WINDOWS_HTTP_OK') { throw 'Unexpected host time endpoint.' }
    $hostUtc = [DateTimeOffset]::ParseExact([string]$response.Headers['Date'], 'r', [Globalization.CultureInfo]::InvariantCulture).UtcDateTime
    $script:lastHostUtc = $hostUtc
    $script:lastHostTick = [Diagnostics.Stopwatch]::GetTimestamp()
    $guestUtc = (Get-Date).ToUniversalTime()
    return @{ hostUtc = $hostUtc.ToString('o'); guestUtc = $guestUtc.ToString('o')
              offsetSeconds = ($guestUtc - $hostUtc).TotalSeconds; roundTripMilliseconds = $timer.Elapsed.TotalMilliseconds }
}
function Read-TimeSource {
    $lines = @(& w32tm.exe /query /source 2>&1 | ForEach-Object { "$_" })
    return @{ exitCode = $LASTEXITCODE; lines = $lines }
}
$report = [ordered]@{ schema = 1; kind = 'pcemu-hyperv-clock'; stepRequested = [bool]$AllowClockStep
    adjustmentObserved = $false; restoredByProbe = $false; error = $null; cleanupErrors = @() }
$ntpChanged = $false; $timeStopped = $false; $stepped = $false; $timeWasRunning = $false; $serviceWasRunning = $false
try {
    $report.before = Read-ClockSample
    $report.sourceBefore = Read-TimeSource
    if ($AllowClockStep) {
        # Integrity SID is language-independent; a filtered administrator token
        # cannot step the clock. Both high-integrity admin and SYSTEM are valid.
        $groups = (& whoami.exe /groups /fo csv /nh 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0 -or $groups -notmatch 'S-1-16-(12288|16384)') { throw 'An elevated token is required.' }
        $serviceWasRunning = (Get-Service vmictimesync).Status -eq 'Running'
        if (-not $serviceWasRunning) { throw 'Hyper-V timesync must already be running.' }
        $timeWasRunning = (Get-Service w32time).Status -eq 'Running'
        $ntpEnabled = (Get-ItemProperty -LiteralPath $ntpPath -Name Enabled).Enabled
        $vmicEnabled = (Get-ItemProperty -LiteralPath $vmicPath -Name Enabled).Enabled
        if ($vmicEnabled -ne 1) { throw 'VMICTimeProvider must be enabled.' }
        $report.ntpClientEnabledBefore = $ntpEnabled
        Stop-Service vmictimesync
        Stop-Service w32time
        $timeStopped = $true
        Set-ItemProperty -LiteralPath $ntpPath -Name Enabled -Value 0
        $ntpChanged = $true
        Start-Service w32time
        $report.ntpClientDisabled = (Get-ItemProperty -LiteralPath $ntpPath -Name Enabled).Enabled -eq 0
        if (-not $report.ntpClientDisabled) { throw 'NTP client isolation failed.' }
        Set-Date -Adjust ([TimeSpan]::FromSeconds(-120)) | Out-Null
        $stepped = $true
        $report.stepped = Read-ClockSample
        if ([Math]::Abs($report.stepped.offsetSeconds) -lt 60) { throw 'Clock step was not observable.' }
        Start-Service vmictimesync
        $report.samples = @()
        for ($i = 0; $i -lt 12; $i++) {
            Start-Sleep -Seconds 5
            $sample = Read-ClockSample
            $report.samples += $sample
            if ($sample.roundTripMilliseconds -le 2000 -and [Math]::Abs($sample.offsetSeconds) -le 3) { break }
        }
        $report.after = $report.samples[-1]
        $report.sourceAfter = Read-TimeSource
        $report.ntpClientDisabledAfter = (Get-ItemProperty -LiteralPath $ntpPath -Name Enabled).Enabled -eq 0
        $report.adjustmentObserved = ($report.ntpClientDisabledAfter -and $report.after.roundTripMilliseconds -le 2000 -and
            [Math]::Abs($report.after.offsetSeconds) -le 3 -and $report.sourceAfter.exitCode -eq 0 -and
            ($report.sourceAfter.lines -join ' ') -match 'VM IC Time Synchronization Provider')
    }
} catch { $report.error = $_.Exception.Message }
finally {
    if ($stepped -and -not $report.adjustmentObserved) {
        try {
            try {
                $sample = Read-ClockSample
                $restoreUtc = [DateTime]::Parse($sample.hostUtc)
            } catch {
                if ($null -eq $script:lastHostUtc) { throw }
                $elapsed = ([Diagnostics.Stopwatch]::GetTimestamp() - $script:lastHostTick) / [double][Diagnostics.Stopwatch]::Frequency
                $restoreUtc = $script:lastHostUtc.AddSeconds($elapsed)
            }
            Set-Date -Date $restoreUtc.ToLocalTime() | Out-Null
            $report.restoredByProbe = $true
        } catch { $report.cleanupErrors += $_.Exception.Message }
    }
    if ($timeStopped) {
        try {
            Stop-Service w32time
            if ($ntpChanged) { Set-ItemProperty -LiteralPath $ntpPath -Name Enabled -Value $ntpEnabled }
            if ($timeWasRunning) { Start-Service w32time }
        } catch { $report.cleanupErrors += $_.Exception.Message }
    }
    if ($AllowClockStep -and $serviceWasRunning) {
        try { Start-Service vmictimesync } catch { $report.cleanupErrors += $_.Exception.Message }
    }
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Output "PCEMU_HYPERV_CLOCK_REPORT=$OutputPath"
if ($report.error -or $report.cleanupErrors.Count -or ($AllowClockStep -and -not $report.adjustmentObserved)) { exit 1 }
