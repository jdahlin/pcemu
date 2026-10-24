@echo off
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoAdminLogon /t REG_SZ /d 0 /f >nul 2>&1
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultPassword /f >nul 2>&1
set PCEMU_FIRST_LOGON=1
call C:\PCEMU\FIRSTBOOT.CMD
powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\PCEMU\CAPTURE-DESKTOP.PS1 > C:\PCEMU\DESKTOP.LOG 2>&1
C:\PCEMU\HTTP.EXE post C:\PCEMU\DESKTOP.LOG
if exist C:\PCEMU\DESKTOP.PNG C:\PCEMU\HTTP.EXE post C:\PCEMU\DESKTOP.PNG
