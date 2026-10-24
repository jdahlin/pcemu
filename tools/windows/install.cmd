@echo off
ver
echo PCEMU_INSTALL_BEGIN
wpeinit
drvload X:\PCEMU\NETKVM\netkvm.inf
if errorlevel 1 goto fail
drvload X:\PCEMU\DRIVERS\viostor\viostor.inf
if errorlevel 1 goto fail
wpeutil InitializeNetwork
ipconfig /all
X:\PCEMU\VERIFY.EXE
if errorlevel 1 goto fail
X:\PCEMU\HTTP.EXE post X:\PCEMU\INSTALL.LOG
diskpart /s X:\PCEMU\PARTITION.TXT
if errorlevel 1 goto fail
if not exist W:\ goto fail
if not exist S:\ goto fail
mkdir W:\Scratch
wpeutil CreatePageFile /path=W:\pagefile.sys /size=256
echo PCEMU_PHASE_DOWNLOAD
X:\PCEMU\HTTP.EXE post X:\PCEMU\INSTALL.LOG
X:\PCEMU\HTTP.EXE get /install.wim W:\install.wim
if errorlevel 1 goto fail
echo PCEMU_PHASE_APPLY
X:\PCEMU\HTTP.EXE post X:\PCEMU\INSTALL.LOG
dism /Apply-Image /ImageFile:W:\install.wim /Index:3 /ApplyDir:W:\ /Compact /CheckIntegrity /Verify /ScratchDir:W:\Scratch
if errorlevel 1 goto fail
echo PCEMU_PHASE_DRIVERS
X:\PCEMU\HTTP.EXE post X:\PCEMU\INSTALL.LOG
dism /Image:W:\ /Add-Driver /Driver:X:\PCEMU\DRIVERS /Recurse /ScratchDir:W:\Scratch
if errorlevel 1 goto fail
dism /Image:W:\ /Add-Driver /Driver:X:\PCEMU\NETKVM\netkvm.inf /ScratchDir:W:\Scratch
if errorlevel 1 goto fail
mkdir W:\PCEMU
copy X:\PCEMU\HTTP.EXE W:\PCEMU\HTTP.EXE
if errorlevel 1 goto fail
copy X:\PCEMU\FIRSTBOOT.CMD W:\PCEMU\FIRSTBOOT.CMD
if errorlevel 1 goto fail
mkdir W:\Windows\Panther
copy X:\PCEMU\UNATTEND.XML W:\Windows\Panther\Unattend.xml
if errorlevel 1 goto fail
bcdboot W:\Windows /s S: /f UEFI
if errorlevel 1 goto fail
del W:\install.wim
echo PCEMU_INSTALL_APPLIED_AND_BOOT_CONFIGURED
X:\PCEMU\HTTP.EXE post X:\PCEMU\INSTALL.LOG
exit /b 0
:fail
echo PCEMU_INSTALL_FAILED RC=%errorlevel%
X:\PCEMU\HTTP.EXE post X:\PCEMU\INSTALL.LOG
exit /b 1
