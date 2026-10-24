@echo off
call :report > C:\PCEMU\FIRSTBOOT.LOG 2>&1
C:\PCEMU\HTTP.EXE post C:\PCEMU\FIRSTBOOT.LOG
exit /b
:report
reg query HKLM\SYSTEM\CurrentControlSet\Control\MiniNT >nul 2>&1
if not errorlevel 1 exit /b 1
echo PCEMU_INSTALLED_WINDOWS_BOOT
ver
if "%PCEMU_FIRST_LOGON%"=="1" echo PCEMU_FIRST_LOGON=%USERNAME%
whoami
echo SYSTEMDRIVE=%SystemDrive%
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v CurrentBuildNumber
bcdedit /enum {current}
ipconfig /renew
ipconfig /all
pnputil /enum-devices /class SCSIAdapter
pnputil /enum-devices /class Net
pnputil /enum-devices /class System
C:\PCEMU\RNG.EXE
echo RNG_CHECK_RC=%errorlevel%
dism /Online /Get-Drivers
if exist C:\PCEMU\HTTP-BOOT.TXT del C:\PCEMU\HTTP-BOOT.TXT
C:\PCEMU\HTTP.EXE get /probe C:\PCEMU\HTTP-BOOT.TXT
echo HTTP_GET_RC=%errorlevel%
type C:\PCEMU\HTTP-BOOT.TXT
echo PCEMU_INSTALLED_WINDOWS_REPORT_END
exit /b 0
