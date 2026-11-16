@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul

rem Select per-board(-variant) build cache. Does NOT require moving a locked root build\.
rem Writes .build-slots\env.cmd for callers:
rem   set "NODELIO_BUILD_DIR=..."
rem   set "NODELIO_SDKCONFIG=..."
rem   set "NODELIO_SLOT=..."
rem
rem Callers should:
rem   call scripts\nodelio_activate_build_slot.bat <slot>
rem   call .build-slots\env.cmd
rem   idf.py -B "!NODELIO_BUILD_DIR!" -D SDKCONFIG="!NODELIO_SDKCONFIG!" build
rem
rem OTA sync: prefers !NODELIO_BUILD_DIR!\xiaozhi.bin (see nodelio_sync_panel_ota.py --build-dir)

set "WANT=%~1"
if "!WANT!"=="" (
  echo [build-slot] ERROR: slot name required
  exit /b 1
)

set "REPO=%~dp0.."
for %%I in ("!REPO!") do set "REPO=%%~fI"
set "SLOT_ROOT=!REPO!\.build-slots"
set "CUR_FILE=!SLOT_ROOT!\CURRENT"
set "ENV_FILE=!SLOT_ROOT!\env.cmd"
set "SLOT_DIR=!SLOT_ROOT!\!WANT!"
set "BD=!SLOT_DIR!\build"
set "SC=!SLOT_DIR!\sdkconfig"
set "CUR="

if not exist "!SLOT_ROOT!" mkdir "!SLOT_ROOT!" >nul 2>&1
if not exist "!SLOT_DIR!" mkdir "!SLOT_DIR!" >nul 2>&1
if not exist "!BD!" mkdir "!BD!" >nul 2>&1

if exist "!CUR_FILE!" (
  for /f "usebackq delims=" %%a in ("!CUR_FILE!") do set "CUR=%%a"
)

rem Seed empty slot from root build\ once (best-effort; ignore locks)
if not exist "!BD!\CMakeCache.txt" (
  if exist "!REPO!\build\CMakeCache.txt" (
    echo [build-slot] seed !WANT! from root build\ ^(robocopy^)
    robocopy "!REPO!\build" "!BD!" /E /COPY:DAT /R:1 /W:1 /NFL /NDL /NJH /NJS >nul
    set "RC=!ERRORLEVEL!"
    if !RC! GEQ 8 (
      echo [build-slot] WARN robocopy seed failed code !RC! — slot starts empty
    ) else (
      echo [build-slot] seeded object cache into .build-slots\!WANT!\build
    )
  )
)

rem Park outgoing sdkconfig into previous slot; bring in WANT sdkconfig
if not "!CUR!"=="" if /I not "!CUR!"=="!WANT!" (
  if exist "!REPO!\sdkconfig" (
    if not exist "!SLOT_ROOT!\!CUR!" mkdir "!SLOT_ROOT!\!CUR!" >nul 2>&1
    move /Y "!REPO!\sdkconfig" "!SLOT_ROOT!\!CUR!\sdkconfig" >nul 2>&1
  )
  if exist "!REPO!\sdkconfig.old" (
    move /Y "!REPO!\sdkconfig.old" "!SLOT_ROOT!\!CUR!\sdkconfig.old" >nul 2>&1
  )
)
if exist "!SC!" (
  copy /Y "!SC!" "!REPO!\sdkconfig" >nul
  echo [build-slot] restored sdkconfig from !WANT!
) else if exist "!REPO!\sdkconfig" (
  rem keep root sdkconfig; will be saved into this slot on next switch
  echo [build-slot] using current sdkconfig for !WANT!
)

> "!CUR_FILE!" echo !WANT!
> "!ENV_FILE!" echo set "NODELIO_SLOT=!WANT!"
>> "!ENV_FILE!" echo set "NODELIO_BUILD_DIR=!BD!"
>> "!ENV_FILE!" echo set "NODELIO_SDKCONFIG=!SC!"

rem Keep a working copy of sdkconfig inside the slot for -D SDKCONFIG=
if exist "!REPO!\sdkconfig" (
  copy /Y "!REPO!\sdkconfig" "!SC!" >nul
)

echo [build-slot] active: !WANT!
echo [build-slot] BUILD_DIR=!BD!
if exist "!BD!\xiaozhi.bin" (echo [build-slot] xiaozhi.bin ready) else (echo [build-slot] no xiaozhi.bin yet — first full build for this slot)
exit /b 0
