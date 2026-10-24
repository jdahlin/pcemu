@echo off
ver
echo PCEMU_FINALIZE_BEGIN
wpeinit
drvload X:\PCEMU\NETKVM\netkvm.inf
if errorlevel 1 goto fail
drvload X:\PCEMU\DRIVERS\viostor\viostor.inf
if errorlevel 1 goto fail
wpeutil InitializeNetwork
diskpart /s X:\PCEMU\FINALIZE-MOUNT.TXT
if errorlevel 1 goto fail
if not exist W:\Windows\System32\ntoskrnl.exe goto fail
if not exist W:\PCEMU\FIRSTBOOT.CMD goto fail
copy X:\PCEMU\RNG.EXE W:\PCEMU\RNG.EXE
if errorlevel 1 goto fail
copy X:\PCEMU\FIRSTBOOT.CMD W:\PCEMU\FIRSTBOOT.CMD
if errorlevel 1 goto fail
copy X:\PCEMU\FIRSTLOGON.CMD W:\PCEMU\FIRSTLOGON.CMD
if errorlevel 1 goto fail
copy X:\PCEMU\CAPTURE-DESKTOP.PS1 W:\PCEMU\CAPTURE-DESKTOP.PS1
if errorlevel 1 goto fail
copy X:\PCEMU\ACCOUNT-UNATTEND.XML W:\Windows\Panther\Unattend.xml
if errorlevel 1 goto fail
bcdboot W:\Windows /s S: /f UEFI
if errorlevel 1 goto fail
if not exist S:\EFI\Boot mkdir S:\EFI\Boot
if not exist S:\EFI\Boot\BOOTAA64.EFI copy W:\Windows\Boot\EFI\bootmgfw.efi S:\EFI\Boot\BOOTAA64.EFI
if not exist S:\EFI\Boot\BOOTAA64.EFI goto fail
dism /Image:W:\ /Get-Drivers
if errorlevel 1 goto fail
echo PCEMU_FINALIZE_OK
exit /b 0
:fail
echo PCEMU_FINALIZE_FAILED RC=%errorlevel%
exit /b 1
