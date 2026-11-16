@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul

rem Promote current build/xiaozhi.bin to stable + upload (no rebuild).
rem   nodelio_promote_panel_ota.bat --board sila_p4c5_62 --variant 32m
rem   nodelio_promote_panel_ota.bat --board sila_86 yes
rem   nodelio_promote_panel_ota.bat --board sila_p4c5_508 --variant 16m --no-upload

set "REPO=D:\xz25"
set "BOARD="
set "VARIANT="
set "NO_UPLOAD=0"
set "AUTO=yes"

:parse
if "%~1"=="" goto :parsed
if /I "%~1"=="--board" (
  set "BOARD=%~2"
  shift
  shift
  goto :parse
)
if /I "%~1"=="--variant" (
  set "VARIANT=%~2"
  shift
  shift
  goto :parse
)
if /I "%~1"=="--no-upload" set "NO_UPLOAD=1"& shift& goto :parse
if /I "%~1"=="yes" set "AUTO=yes"& shift& goto :parse
if /I "%~1"=="y" set "AUTO=yes"& shift& goto :parse
if /I "%~1"=="no" set "AUTO=no"& shift& goto :parse
echo [promote] unknown arg: %~1
echo [promote] usage: --board sila_86^|sila_p4c5_62^|sila_p4c5_508 [--variant 16m^|32m] [--no-upload] [yes^|no]
exit /b 1
:parsed

if "!BOARD!"=="" (
  echo [promote] ERROR: --board required
  exit /b 1
)
if /I "!BOARD!"=="sila_86" (
  set "VARIANT=none"
) else (
  if "!VARIANT!"=="" (
    echo [promote] ERROR: P4 requires --variant 16m^|32m
    exit /b 1
  )
)

set "SLOT="
if /I "!BOARD!"=="sila_86" set "SLOT=sila_86"
if /I "!BOARD!"=="sila_p4c5_62" set "SLOT=sila_p4c5_62_!VARIANT!"
if /I "!BOARD!"=="sila_p4c5_508" set "SLOT=sila_p4c5_508_!VARIANT!"
if not "!SLOT!"=="" call "!REPO!\scripts\nodelio_activate_build_slot.bat" !SLOT!
if exist "!REPO!\.build-slots\env.cmd" call "!REPO!\.build-slots\env.cmd"
set "BUILD_BIN="
if not "!NODELIO_BUILD_DIR!"=="" if exist "!NODELIO_BUILD_DIR!\xiaozhi.bin" set "BUILD_BIN=!NODELIO_BUILD_DIR!\xiaozhi.bin"
if "!BUILD_BIN!"=="" if exist "!REPO!\build\xiaozhi.bin" set "BUILD_BIN=!REPO!\build\xiaozhi.bin"
if "!BUILD_BIN!"=="" (
  echo [promote] ERROR: missing xiaozhi.bin — build/flash first
  exit /b 1
)
for %%I in ("!BUILD_BIN!") do set "BUILD_DIR=%%~dpI"
if "!BUILD_DIR:~-1!"=="\" set "BUILD_DIR=!BUILD_DIR:~0,-1!"

echo [promote] sync stable board=!BOARD! variant=!VARIANT! from !BUILD_BIN! ...
if /I "!BOARD!"=="sila_86" (
  python "!REPO!\scripts\nodelio_sync_panel_ota.py" --root "!REPO!" --board sila_86 --variant none --channel stable --build-dir "!BUILD_DIR!"
) else (
  python "!REPO!\scripts\nodelio_sync_panel_ota.py" --root "!REPO!" --board !BOARD! --variant !VARIANT! --channel stable --build-dir "!BUILD_DIR!"
)
if errorlevel 1 (
  echo [promote] sync failed
  exit /b 1
)

if "!NO_UPLOAD!"=="1" (
  echo [promote] --no-upload: local only
  exit /b 0
)

if /I "!BOARD!"=="sila_86" (
  call "!REPO!\scripts\nodelio_upload_panel_ota.bat" --board sila_86 --variant none --channel stable !AUTO!
) else (
  call "!REPO!\scripts\nodelio_upload_panel_ota.bat" --board !BOARD! --variant !VARIANT! --channel stable !AUTO!
)
if errorlevel 1 (
  echo [promote] WARN upload failed ^(local sync ok^)
  exit /b 0
)
exit /b 0
