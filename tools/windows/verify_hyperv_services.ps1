# Run in installed Windows PowerShell 5.1 or later. StartHeartbeat is an
# explicit runtime experiment; the default only observes the existing state.
[CmdletBinding()]
param(
    [string]$OutputPath = 'C:\PCEMU\HYPERV-SERVICES.JSON',
    [switch]$StartHeartbeat
)
$ErrorActionPreference = 'Stop'
$probeStarted = Get-Date
if (Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Control\MiniNT') {
    throw 'Run this probe in installed Windows, not WinPE.'
}

function Read-ServiceState([string]$Name) {
    $class = if ($Name -eq 'vmbus') { 'Win32_SystemDriver' } else { 'Win32_Service' }
    $service = Get-CimInstance $class -Filter "Name='$Name'"
    if ($null -eq $service) { return @{ name = $Name; present = $false } }
    return @{
        name = $Name; present = $true; state = $service.State
        startMode = $service.StartMode; processId = $service.ProcessId
        exitCode = $service.ExitCode; serviceSpecificExitCode = $service.ServiceSpecificExitCode
    }
}
function Read-NativeCommand([string]$Program, [string[]]$Arguments) {
    $lines = @(& $Program @Arguments 2>&1 | ForEach-Object { "$_" })
    return @{ exitCode = $LASTEXITCODE; lines = $lines }
}

$report = [ordered]@{
    schema = 1; kind = 'pcemu-hyperv-services'; utc = [DateTime]::UtcNow.ToString('o')
    installedWindows = $true; startRequested = [bool]$StartHeartbeat
    before = Read-ServiceState 'vmicheartbeat'
}
# Keep privilege-query failure separate from service activation failure.
$report.privilege = @{ elevated = $null; error = $null }
try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        $principal = New-Object Security.Principal.WindowsPrincipal($identity)
        $report.privilege.elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    } finally { $identity.Dispose() }
} catch { $report.privilege.error = $_.Exception.Message }
$report.activation = @{ attempted = $false; succeeded = $false; error = $null; errors = @(); errorId = $null }
if ($StartHeartbeat) {
    $report.activation.attempted = $true
    try {
        $service = Get-Service -Name vmicheartbeat
        # Start-Service can replace a native ServiceController error with a
        # generic ServiceCommandException without retaining its InnerException.
        if ($service.Status -ne [System.ServiceProcess.ServiceControllerStatus]::Running) { $service.Start() }
        $service.WaitForStatus([System.ServiceProcess.ServiceControllerStatus]::Running,
                               [TimeSpan]::FromSeconds(15))
        $report.activation.succeeded = $true
    } catch {
        $report.activation.error = $_.Exception.Message
        $report.activation.errorId = $_.FullyQualifiedErrorId
        $exception = $_.Exception
        $chain = @()
        # PowerShell wraps ServiceController failures; the Win32 error can be
        # several InnerExceptions below its generic service-start message.
        for ($depth = 0; $null -ne $exception -and $depth -lt 8; $depth++) {
            $nativeCode = $null
            if ($exception -is [ComponentModel.Win32Exception]) { $nativeCode = $exception.NativeErrorCode }
            $chain += @{ type = $exception.GetType().FullName; message = $exception.Message
                         hresult = $exception.HResult; nativeErrorCode = $nativeCode }
            $exception = $exception.InnerException
        }
        $report.activation.errors = $chain
    }
}
$report.after = Read-ServiceState 'vmicheartbeat'
$report.services = @(foreach ($name in @('vmicheartbeat', 'vmictimesync', 'vmicshutdown', 'vmbus')) {
    Read-ServiceState $name
})
$report.processes = @(Get-Process -Name services, wininit, winlogon, explorer -ErrorAction SilentlyContinue |
    Select-Object ProcessName, Id, SessionId)
$report.controllerStatus = Read-NativeCommand 'sc.exe' @('queryex', 'vmicheartbeat')
$report.serviceConfiguration = Read-NativeCommand 'sc.exe' @('qc', 'vmicheartbeat')
$report.events = @{ records = @(); error = $null }
try {
    # Read live events before guest log buffers can be lost on host stop. Bound
    # both the time interval and count; retain XML to avoid localized rendering.
    $events = @(Get-WinEvent -FilterHashtable @{
        LogName = 'System'; ProviderName = 'Service Control Manager'
        StartTime = $probeStarted.AddMinutes(-1)
    } -MaxEvents 64 -ErrorAction Stop)
    $report.events.records = @($events | ForEach-Object {
        $xml = $_.ToXml()
        if ($xml -match 'vmicheartbeat|Hyper-V Heartbeat') { $xml }
    })
} catch { $report.events.error = $_.Exception.Message }
$report.trigger = Read-NativeCommand 'sc.exe' @('qtriggerinfo', 'vmicheartbeat')
# The device instance uses the interface GUID and our deterministic instance GUID.
$instance = 'VMBUS\{57164f39-9115-4e78-ab55-382f3bd5422d}\{fd149e91-82e0-4a7d-afa6-2a4166cbd7c0}'
$report.device = Read-NativeCommand 'pnputil.exe' @('/enum-devices', '/instanceid', $instance, '/properties', '/services', '/interfaces')
$report.channelUseVerified = $false # Must be correlated with host GPADL/ring/heartbeat counters.
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Output "PCEMU_HYPERV_SERVICES_REPORT=$OutputPath"
if ($StartHeartbeat -and -not $report.activation.succeeded) { exit 1 }
