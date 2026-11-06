@echo off
rem Workspace temp/cache so go build does not hit a locked system temp.
if not exist "%~dp0tmp\gotmp" mkdir "%~dp0tmp\gotmp"
if not exist "%~dp0tmp\gocache" mkdir "%~dp0tmp\gocache"
set "GOTMPDIR=%~dp0tmp\gotmp"
set "TMP=%~dp0tmp\gotmp"
set "TEMP=%~dp0tmp\gotmp"
set "GOCACHE=%~dp0tmp\gocache"
set "AIR_EXE=%~dp0air.exe"
cd /d "%~dp0"
rem air.exe was marked Low integrity, so Windows dropped the process and go could not create directories.
icacls "%AIR_EXE%" /setintegritylevel M >nul
powershell -NoProfile -Command "if (Get-CimInstance Win32_Process -Filter \"Name='air.exe'\" | Where-Object { $_.ExecutablePath -eq $env:AIR_EXE }) { exit 1 } else { exit 0 }"
if errorlevel 1 (
  echo [air-run] air already running. Close that window first.
  exit /b 1
)
rem This program keeps a single-instance lock. A bridge left by the previous air must exit first.
set "BRIDGE_EXE=%~dp0tmp\nodelio-bridge.exe"
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='nodelio-bridge.exe'\" | Where-Object { $_.ExecutablePath -eq $env:BRIDGE_EXE } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"
"%~dp0air.exe" %*
