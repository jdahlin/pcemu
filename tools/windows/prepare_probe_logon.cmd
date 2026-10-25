@echo off
rem WinPE only: W: is the private installed-Windows test volume.
rem Caller has loaded its SOFTWARE hive as HKLM\PCEMU_SOFT and staged private
rem test-account credentials. Never query or print DefaultPassword here.
reg query "HKLM\PCEMU_SOFT\Microsoft\Windows NT\CurrentVersion" >nul 2>&1
if errorlevel 1 goto fail
reg load HKLM\PCEMU_PROBE_SYS W:\Windows\System32\config\SYSTEM >nul 2>&1
if errorlevel 1 goto fail
set PCEMU_PROBE_CS=
for /f "tokens=3" %%A in ('reg query HKLM\PCEMU_PROBE_SYS\Select /v Current 2^>nul') do set /a PCEMU_PROBE_CS=%%A >nul
if not defined PCEMU_PROBE_CS goto unloadfail
set PCEMU_PROBE_CS=00%PCEMU_PROBE_CS%
set PCEMU_PROBE_CS=%PCEMU_PROBE_CS:~-3%
set PCEMU_PROBE_DOMAIN=
for /f "tokens=3" %%A in ('reg query HKLM\PCEMU_PROBE_SYS\ControlSet%PCEMU_PROBE_CS%\Control\ComputerName\ComputerName /v ComputerName 2^>nul') do set PCEMU_PROBE_DOMAIN=%%A
if not defined PCEMU_PROBE_DOMAIN goto unloadfail
reg add "HKLM\PCEMU_SOFT\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultDomainName /t REG_SZ /d "%PCEMU_PROBE_DOMAIN%" /f >nul 2>&1
if errorlevel 1 goto unloadfail
reg add "HKLM\PCEMU_SOFT\Microsoft\Windows NT\CurrentVersion\Winlogon" /v ForceAutoLogon /t REG_SZ /d 1 /f >nul 2>&1
if errorlevel 1 goto unloadfail
reg add "HKLM\PCEMU_SOFT\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoLogonCount /t REG_DWORD /d 1 /f >nul 2>&1
if errorlevel 1 goto unloadfail
reg unload HKLM\PCEMU_PROBE_SYS >nul 2>&1
if errorlevel 1 goto fail
echo PCEMU_PROBE_LOGON_DOMAIN_OK
exit /b 0
:unloadfail
reg unload HKLM\PCEMU_PROBE_SYS >nul 2>&1
:fail
echo PCEMU_PROBE_LOGON_DOMAIN_FAILED
exit /b 1
