# Cross-platform control-flow checks with mocked Windows service/PnP APIs.
$ErrorActionPreference = 'Stop'
$probe = Join-Path $PSScriptRoot '../tools/windows/verify_hyperv_services.ps1'
$tokens = $null; $errors = $null
[System.Management.Automation.Language.Parser]::ParseFile($probe, [ref]$tokens, [ref]$errors) | Out-Null
if ($errors.Count) { throw ($errors | Out-String) }
$global:pcemuProbeState = 'Stopped'; $global:pcemuProbeStarts = 0; $global:pcemuProbeFailStart = $false
$global:pcemuProbeEventsFail = $false
function Test-Path { param($Path) return $false }
function Get-CimInstance {
    param($ClassName, $Filter)
    $expected = if ($Filter -eq "Name='vmbus'") { 'Win32_SystemDriver' } else { 'Win32_Service' }
    if ($ClassName -ne $expected) { throw 'Unexpected CIM query' }
    [pscustomobject]@{ State = $global:pcemuProbeState; StartMode = 'Manual'; ProcessId = 0; ExitCode = 0; ServiceSpecificExitCode = 0 }
}
function Get-WinEvent {
    param($FilterHashtable, $MaxEvents, $ErrorAction)
    if ($MaxEvents -ne 64 -or $FilterHashtable.ProviderName -ne 'Service Control Manager') {
        throw 'Unbounded or incorrect event query'
    }
    if ($global:pcemuProbeEventsFail) { throw 'Mock event log unavailable' }
    $event = [pscustomobject]@{}
    $event | Add-Member ScriptMethod ToXml { '<Event><Data>vmicheartbeat</Data><Error>5</Error></Event>' }
    return $event
}
function Get-Process { param($Name, $ErrorAction) return @() }
function Get-Service {
    param($Name)
    if ($Name -ne 'vmicheartbeat') { throw 'Unexpected service mutation' }
    $s = [pscustomobject]@{ Status = $global:pcemuProbeState }
    $s | Add-Member ScriptMethod Start {
        $global:pcemuProbeStarts++
        if ($global:pcemuProbeFailStart) {
            throw [InvalidOperationException]::new('Mock activation denied', [ComponentModel.Win32Exception]::new(5))
        }
        $global:pcemuProbeState = 'Running'
    }
    $s | Add-Member ScriptMethod WaitForStatus {
        param($State, $Timeout)
        if ($State.ToString() -ne 'Running' -or $Timeout.TotalSeconds -ne 15) { throw 'Unbounded service wait' }
    }
    return $s
}
function sc.exe { $global:LASTEXITCODE = 0; 'Mock trigger information' }
function pnputil.exe { $global:LASTEXITCODE = 0; 'Mock device information' }
$path = Join-Path ([IO.Path]::GetTempPath()) ('pcemu-hyperv-' + [guid]::NewGuid() + '.json')
try {
    & $probe -OutputPath $path
    $r = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    if ($global:pcemuProbeStarts -ne 0 -or $r.activation.attempted -or $r.startRequested -or $r.channelUseVerified) {
        throw 'Read-only probe mutated service or claimed channel use'
    }
    if ($r.schema -ne 1 -or $r.services.Count -ne 4 -or $r.after.state -ne 'Stopped') { throw ('Incomplete state report: ' + ($r | ConvertTo-Json -Depth 8)) }
    & $probe -OutputPath $path -StartHeartbeat
    $r = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    if ($global:pcemuProbeStarts -ne 1 -or -not $r.activation.succeeded -or $r.after.state -ne 'Running' -or $r.channelUseVerified) {
        throw 'Successful activation report incorrect'
    }
    & $probe -OutputPath $path -StartHeartbeat
    if ($global:pcemuProbeStarts -ne 1) { throw 'Already running service was started again' }
    $global:pcemuProbeState = 'Stopped'; $global:pcemuProbeFailStart = $true
    & $probe -OutputPath $path -StartHeartbeat
    $r = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    if ($LASTEXITCODE -ne 1 -or $r.activation.succeeded -or $r.activation.error -notlike '*Mock activation denied*') {
        throw 'Failed activation not preserved in report and exit status'
    }
    if ($r.events.records.Count -ne 1 -or $r.events.records[0] -notmatch 'vmicheartbeat') {
        throw 'Matching live service event was not retained'
    }
    $native = @($r.activation.errors | Where-Object { $_.nativeErrorCode -eq 5 })
    if ($native.Count -ne 1 -or $r.activation.errors.Count -lt 2 -or -not $r.activation.errorId) {
        throw 'Wrapped native service error was lost'
    }
    $global:pcemuProbeEventsFail = $true
    & $probe -OutputPath $path
    $r = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    if ($r.events.error -ne 'Mock event log unavailable' -or $r.events.records.Count -ne 0 -or $r.activation.attempted) {
        throw 'Event-log failure obscured the observation report'
    }
    Write-Output 'Hyper-V service probe: parser, observation, activation and failure checks passed'
} finally { Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue }
