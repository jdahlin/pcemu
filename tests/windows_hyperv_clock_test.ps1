$ErrorActionPreference = 'Stop'
$probe = Join-Path $PSScriptRoot '../tools/windows/verify_hyperv_clock.ps1'
$tokens = $null; $errors = $null
[System.Management.Automation.Language.Parser]::ParseFile($probe, [ref]$tokens, [ref]$errors) | Out-Null
if ($errors.Count) { throw ($errors | Out-String) }
$global:pcemuHostUtc = [DateTime]::Parse('2026-10-25T16:00:00Z').ToUniversalTime()
function Reset-Fixture {
    $global:pcemuGuestUtc = $global:pcemuHostUtc
    $global:pcemuNtpEnabled = 1; $global:pcemuSets = 0; $global:pcemuWrites = 0
    $global:pcemuCorrect = $true; $global:pcemuFailNtpWrite = $false
    $global:pcemuBinaryResponse = $false; $global:pcemuBadEndpoint = $false
    $global:pcemuServices = @{ vmictimesync = 'Running'; w32time = 'Running' }
}
function Test-Path { param($Path) return $false }
function Get-Date { return $global:pcemuGuestUtc }
function Invoke-WebRequest {
    param($Uri, [switch]$UseBasicParsing, $TimeoutSec)
    if ($Uri -ne 'http://10.0.2.2:8080/probe' -or $TimeoutSec -ne 5) { throw 'Unexpected endpoint' }
    $content = if ($global:pcemuBadEndpoint) { 'not the host endpoint' } else { "PCEMU_WINDOWS_HTTP_OK`r`n" }
    if ($global:pcemuBinaryResponse) { $content = [Text.Encoding]::UTF8.GetBytes($content) }
    return @{ Content = $content; Headers = @{ Date = $global:pcemuHostUtc.ToString('r') } }
}
function w32tm.exe { $global:LASTEXITCODE = 0; if ($global:pcemuCorrect) { 'VM IC Time Synchronization Provider' } else { 'Local CMOS Clock' } }
function whoami.exe { $global:LASTEXITCODE = 0; 'S-1-16-12288' }
function Get-Service { param($Name) return [pscustomobject]@{ Status = $global:pcemuServices[$Name] } }
function Stop-Service { param($Name) $global:pcemuServices[$Name] = 'Stopped' }
function Start-Service {
    param($Name)
    $global:pcemuServices[$Name] = 'Running'
    if ($Name -eq 'vmictimesync' -and $global:pcemuCorrect) { $global:pcemuGuestUtc = $global:pcemuHostUtc }
}
function Get-ItemProperty {
    param($LiteralPath, $Name)
    if ($LiteralPath -like '*\NtpClient') { return @{ Enabled = $global:pcemuNtpEnabled } }
    if ($LiteralPath -like '*\VMICTimeProvider') { return @{ Enabled = 1 } }
    throw 'Unexpected registry read'
}
function Set-ItemProperty {
    param($LiteralPath, $Name, $Value)
    if ($LiteralPath -notlike '*\NtpClient' -or $Name -ne 'Enabled') { throw 'Unexpected registry write' }
    if ($global:pcemuFailNtpWrite -and $Value -eq 0) { throw 'Mock registry failure' }
    $global:pcemuWrites++; $global:pcemuNtpEnabled = $Value
}
function Set-Date {
    param($Adjust, $Date)
    $global:pcemuSets++
    if ($null -ne $Adjust) { $global:pcemuGuestUtc = $global:pcemuGuestUtc.Add($Adjust) }
    else { $global:pcemuGuestUtc = $Date.ToUniversalTime() }
}
function Start-Sleep { param($Seconds) if ($Seconds -ne 5) { throw 'Unexpected wait' } }
$path = Join-Path ([IO.Path]::GetTempPath()) ('pcemu-clock-' + [guid]::NewGuid() + '.json')
try {
    Reset-Fixture
    & $probe -OutputPath $path
    $r = Get-Content -Raw $path | ConvertFrom-Json
    if ($global:pcemuSets -or $global:pcemuWrites -or $r.stepRequested -or $r.adjustmentObserved) { throw 'Observation mutated clock' }
    Reset-Fixture; $global:pcemuBinaryResponse = $true
    & $probe -OutputPath $path -AllowClockStep
    $r = Get-Content -Raw $path | ConvertFrom-Json
    if (-not $r.adjustmentObserved -or $r.restoredByProbe -or $r.stepped.offsetSeconds -ne -120 -or $global:pcemuSets -ne 1) { throw ('Correction evidence incorrect: ' + ($r | ConvertTo-Json -Depth 8)) }
    if ($global:pcemuNtpEnabled -ne 1 -or $global:pcemuServices.w32time -ne 'Running') { throw 'Original time policy not restored' }
    Reset-Fixture; $global:pcemuCorrect = $false
    & $probe -OutputPath $path -AllowClockStep
    $r = Get-Content -Raw $path | ConvertFrom-Json
    if ($LASTEXITCODE -ne 1 -or $r.adjustmentObserved -or -not $r.restoredByProbe -or $global:pcemuGuestUtc -ne $global:pcemuHostUtc) { throw 'Probe restoration was mistaken for Hyper-V correction' }
    if ($global:pcemuNtpEnabled -ne 1) { throw 'NTP policy not restored on failure' }
    Reset-Fixture; $global:pcemuFailNtpWrite = $true
    & $probe -OutputPath $path -AllowClockStep
    $r = Get-Content -Raw $path | ConvertFrom-Json
    if ($r.error -ne 'Mock registry failure' -or $global:pcemuServices.w32time -ne 'Running' -or $global:pcemuSets) { throw 'Partial setup left time service stopped' }
    Reset-Fixture; $global:pcemuBadEndpoint = $true
    & $probe -OutputPath $path -AllowClockStep
    $r = Get-Content -Raw $path | ConvertFrom-Json
    if ($r.error -ne 'Unexpected host time endpoint.' -or $global:pcemuSets -or $global:pcemuWrites) { throw 'Bad endpoint reached clock mutation' }
    Write-Output 'Clock probe parser, observation, correction, restoration and setup-failure checks passed'
} finally { Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue }
