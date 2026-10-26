#Requires -RunAsAdministrator
param([Parameter(Mandatory=$true)][string]$Source)
$ErrorActionPreference = 'Stop'
if ($env:PROCESSOR_ARCHITECTURE -ne 'ARM64') { throw 'Run native ARM64 PowerShell' }
$expected = @{
    'vdagent.exe' = 'cc5167af34bf5d6dc594e85d0c309af64c1e22b9f1297a5ef69ca99a403c479a'
    'vdservice.exe' = '4862a0ecd865c3f9ede015df4639ab97a4a166a0580e0644ab2f42052b629060'
}
foreach ($name in $expected.Keys) {
    $path = Join-Path $Source $name
    if ((Get-FileHash -Algorithm SHA256 $path).Hash -ne $expected[$name]) { throw "Hash mismatch: $name" }
}
$destination = Join-Path $env:ProgramFiles 'SPICE Guest Tools'
New-Item -ItemType Directory -Force $destination | Out-Null
foreach ($name in $expected.Keys) { Copy-Item (Join-Path $Source $name) $destination -Force }
$service = Get-Service vdservice -ErrorAction SilentlyContinue
if (!$service) {
    & (Join-Path $destination 'vdservice.exe') install
    if ($LASTEXITCODE) { throw "SPICE service installation failed: $LASTEXITCODE" }
}
# The signed DOD package defaults this supported setting to zero. Enable its
# hardware cursor without changing the INF, catalog or driver binary.
$devices = @(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
if ($devices.Count -ne 1) { throw 'Expected exactly one VirtIO GPU display adapter' }
$driver = (Get-PnpDeviceProperty -InstanceId $devices[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
if ($driver -notmatch '^\{4d36e968-e325-11ce-bfc1-08002be10318\}\\\d{4}$') { throw 'Unexpected display driver registry path' }
$key = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$driver"
New-ItemProperty -Path $key -Name HWCursor -PropertyType DWord -Value 1 -Force | Out-Null
Start-Service vdservice
Get-Service vdservice
Get-ItemProperty -Path $key -Name HWCursor
Write-Output 'PCEMU_SPICE_AGENT_INSTALLED; restart Windows to enable hardware cursor'
