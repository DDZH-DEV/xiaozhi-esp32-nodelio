@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul

rem SILA P4C5 6.2" — P4 host flash only (does NOT program C5 slave)
rem   nodelio-flash.bat                 COM8, 32m, build then flash+monitor
rem   nodelio-flash.bat COM15
rem   nodelio-flash.bat --16m           Flash>=16MiB board (capacity must be confirmed)
rem   nodelio-flash.bat --32m           default (W25Q256 / 32MB board)
rem   nodelio-flash.bat --nofresh       skip build, flash existing build/
rem   nodelio-flash.bat COM15 --16m --nofresh
rem
rem Do NOT use sila-86 nodelio-build.bat. Full flash via flash_args;
rem never treat a lone xiaozhi.bin as a first-flash package.

set "PORT=COM8"
set "NOFRESH=0"
set "VARIANT=32m"
:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--nofresh" (
  set "NOFRESH=1"
  shift
  goto parse
)
if /i "%~1"=="--16m" (
  set "VARIANT=16m"
  shift
  goto parse
)
if /i "%~1"=="--32m" (
  set "VARIANT=32m"
  shift
  goto parse
)
set "PORT=%~1"
shift
goto parse
:parsed

set "BOARD_DIR=sila/sila-p4c5-62"
set "BUILD_NAME=sila-p4c5-62-!VARIANT!"
set "PART_CSV=partitions/nodelio-p4c5-!VARIANT!.csv"

set "REPO=D:\xz25"
set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v6.1"
set "IDF_TOOLS_PATH=D:\Espressif"
set "IDF_GITHUB_ASSETS=dl.espressif.com/github_assets"
set "PATH=D:\Espressif\python_env\idf6.1_py3.11_env\Scripts;D:\Espressif\tools\idf-python\3.11.2;%PATH%"

call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
cd /d "!REPO!"

if "!NOFRESH!"=="1" goto after_build

echo [p4c5-62] idf.py --version
idf.py --version
if errorlevel 1 exit /b 1

echo [p4c5-62] build !BOARD_DIR! --name !BUILD_NAME! ...
python scripts\build.py !BOARD_DIR! --name !BUILD_NAME!
if errorlevel 1 exit /b 1

rem build.py uses subprocess; re-export so PATH sticks for flash
call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
cd /d "!REPO!"

:after_build
if not exist build\flash_args (
  echo [p4c5-62] missing build\flash_args - full first-flash needs flash_args, not app-only
  echo [p4c5-62] run without --nofresh, or: python scripts\build.py !BOARD_DIR! --name !BUILD_NAME!
  exit /b 1
)
if not exist build\xiaozhi.bin (
  echo [p4c5-62] missing build\xiaozhi.bin
  exit /b 1
)

if not exist sdkconfig (
  echo [p4c5-62] missing sdkconfig
  exit /b 1
)
findstr /b /c:"CONFIG_BOARD_TYPE_SILA_P4C5_62=y" sdkconfig >nul
if errorlevel 1 (
  echo [p4c5-62] CONFIG_BOARD_TYPE_SILA_P4C5_62 not set - refuse flash ^(wrong board build^)
  exit /b 1
)
findstr /c:"nodelio-p4c5-!VARIANT!.csv" sdkconfig >nul
if errorlevel 1 (
  echo [p4c5-62] WARN partition table may not match !VARIANT! ^(!PART_CSV!^)
  echo [p4c5-62] 16m/32m layouts are not app-only OTA compatible - rebuild for this variant
)

if /i "!VARIANT!"=="32m" (
  findstr /b /c:"CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y" sdkconfig >nul
  if errorlevel 1 (
    echo [p4c5-62] WARN expected CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y for --32m
  )
)

echo [p4c5-62] flash+monitor on !PORT! ^(P4 USB Serial/JTAG; variant=!VARIANT!; C5 NOT flashed^) ...
idf.py -p !PORT! flash monitor
exit /b %ERRORLEVEL%
