$ErrorActionPreference = 'Stop'
& {
    $text = Get-Clipboard -Raw
    if ($text -ne 'PCEMU_HOST_TO_GUEST_20261026') { throw 'Host clipboard mismatch' }
    Write-Output 'PCEMU_SPICE_HOST_TO_GUEST_OK'
    Set-Clipboard -Value 'PCEMU_GUEST_TO_HOST_20261026'
    Start-Sleep -Seconds 5
    Get-Service vdservice
    Get-Process vdagent
    Write-Output 'PCEMU_SPICE_CLIPBOARD_PROBE_OK'
} *> C:\PCEMU\SPICE-CHECK.LOG
& C:\PCEMU\TOOLS-HTTP.EXE post C:\PCEMU\SPICE-CHECK.LOG
