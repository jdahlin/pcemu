param(
    [string]$DriverRoot = 'C:\PCEMU\VirtIO-ARM64',
    [string]$ReportPath = 'C:\PCEMU\GUEST-DRIVERS-INSTALL.JSON'
)
# Run elevated. Windows validates catalog trust again when pnputil imports each INF.
$ErrorActionPreference = 'Stop'
if ($env:PROCESSOR_ARCHITECTURE -ne 'ARM64') { throw 'Native ARM64 PowerShell is required' }
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run as administrator' }
$root = (Resolve-Path $DriverRoot).Path
$manifest = Get-Content (Join-Path $root 'manifest.json') -Raw | ConvertFrom-Json
foreach ($file in $manifest.files) {
    $path = [IO.Path]::GetFullPath((Join-Path $root $file.path))
    if (!$path.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid manifest path' }
    if ((Get-FileHash $path -Algorithm SHA256).Hash -ne $file.sha256) { throw "Hash mismatch: $path" }
}
$catalogs = @($manifest.files | Where-Object {$_.path -like '*.cat'} | ForEach-Object {Get-Item (Join-Path $root $_.path)} | ForEach-Object {
    $signature = Get-AuthenticodeSignature $_.FullName
    if ($signature.Status -ne 'Valid') { throw "Untrusted catalog: $($_.FullName) $($signature.Status)" }
    [pscustomobject]@{file=$_.FullName;status=[string]$signature.Status;signer=$signature.SignerCertificate.Subject}
})
$before = @(Get-CimInstance Win32_PnPSignedDriver | Where-Object {$_.DeviceID -match 'VEN_1AF4'} |
    Select-Object DeviceName,DeviceID,DriverVersion,InfName,IsSigned)
$activeBlock = @(Get-CimInstance Win32_PnPEntity | Where-Object {$_.Service -eq 'viostor' -and $_.ConfigManagerErrorCode -eq 0})
$results = @()
foreach ($inf in ($manifest.files | Where-Object {$_.path -like '*.inf'} | ForEach-Object {Get-Item (Join-Path $root $_.path)} | Sort-Object FullName)) {
    if ($inf.Name -ieq 'viostor.inf' -and $activeBlock.Count) {
        # Our INTx-only controller requires the working pre-regression boot driver.
        $results += [pscustomobject]@{inf=$inf.FullName;action='preserved-active-boot-driver';exitCode=0;output=''}
        continue
    }
    $output = & pnputil.exe /add-driver $inf.FullName /install 2>&1 | Out-String
    $code = $LASTEXITCODE
    # 259 means no matching device update, or the existing driver ranks better.
    $action = if ($code -eq 259) {'staged-no-device-update'} else {'add-and-match'}
    $results += [pscustomobject]@{inf=$inf.FullName;action=$action;exitCode=$code;output=$output}
    if ($code -notin @(0,259,3010)) {
        $results | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 $ReportPath
        throw "pnputil failed for $($inf.Name): $code"
    }
}
& pnputil.exe /scan-devices | Out-Null
if ($LASTEXITCODE -notin @(0,3010)) { throw 'Device rescan failed' }
$after = @(Get-CimInstance Win32_PnPSignedDriver | Where-Object {$_.DeviceID -match 'VEN_1AF4'} |
    Select-Object DeviceName,DeviceID,DriverVersion,InfName,IsSigned)
$devices = @(Get-CimInstance Win32_PnPEntity | Where-Object {$_.PNPDeviceID -match '^PCI|^VMBUS|DISPLAY|QEMU'} |
    Select-Object Name,PNPDeviceID,Service,ConfigManagerErrorCode,Status)
$store = @(Get-WindowsDriver -Online | Select-Object Driver,OriginalFileName,ProviderName,ClassName,Version)
[pscustomobject]@{catalogs=$catalogs;results=$results;before=$before;after=$after;devices=$devices;driverStore=$store;
    rebootRequired=(@($results | Where-Object exitCode -eq 3010).Count -gt 0)} |
    ConvertTo-Json -Depth 7 | Set-Content -Encoding UTF8 $ReportPath
Write-Host "Driver report: $ReportPath"
