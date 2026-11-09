@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul

rem SILA P4C5 6.2" - P4 host build only (does NOT flash P4 or C5)
rem   nodelio-build.bat          default 32m (W25Q256)，会重新配置
rem   nodelio-build.bat --32m
rem   nodelio-build.bat --16m    Flash>=16MiB board (capacity must be confirmed)
rem   nodelio-build.bat --quick  本板已配过：只增量 idf.py build，不重新 cmake
rem
rem Do NOT use sila-86 nodelio-build.bat. This wraps:
rem   python scripts/build.py sila/sila-p4c5-62 --name sila-p4c5-62-32m

set "VARIANT=32m"
set "QUICK=0"
:parse
if "%~1"=="" goto :parsed
if /I "%~1"=="--16m" set "VARIANT=16m"& shift& goto :parse
if /I "%~1"=="--32m" set "VARIANT=32m"& shift& goto :parse
if /I "%~1"=="--quick" set "QUICK=1"& shift& goto :parse
echo [p4c5-62] unknown arg: %~1
echo [p4c5-62] usage: nodelio-build.bat [--32m^|--16m] [--quick]
exit /b 1
:parsed

set "BOARD_DIR=sila/sila-p4c5-62"
set "BUILD_NAME=sila-p4c5-62-!VARIANT!"

set "REPO=D:\xz25"
set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v6.1"
set "IDF_TOOLS_PATH=D:\Espressif"
set "IDF_GITHUB_ASSETS=dl.espressif.com/github_assets"
set "PATH=D:\Espressif\python_env\idf6.1_py3.11_env\Scripts;D:\Espressif\tools\idf-python\3.11.2;%PATH%"

call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
cd /d "!REPO!"
python "!REPO!\scripts\nodelio_patch_esp_ml307.py"
if errorlevel 1 exit /b 1

echo [p4c5-62] idf.py --version
idf.py --version
if errorlevel 1 exit /b 1

echo [p4c5-62] build stamp ^(sila_p4c5_62 day serial^) ...
python "!REPO!\scripts\nodelio_build_stamp.py" --root "!REPO!" --board sila_p4c5_62
if errorlevel 1 (
  echo [p4c5-62] build stamp failed
  exit /b 1
)

if "!QUICK!"=="1" goto quick_build

echo [p4c5-62] build !BOARD_DIR! --name !BUILD_NAME! ...
echo [p4c5-62] full path reconfigures sdkconfig; UI-only changes: nodelio-build.bat --quick
python scripts\build.py !BOARD_DIR! --name !BUILD_NAME!
if errorlevel 1 exit /b 1
call :sync_ota
exit /b %ERRORLEVEL%

:quick_build
if not exist sdkconfig (
  echo [p4c5-62] no sdkconfig - run once without --quick
  exit /b 1
)
findstr /b /c:"CONFIG_BOARD_TYPE_SILA_P4C5_62=y" sdkconfig >nul
if errorlevel 1 (
  echo [p4c5-62] sdkconfig is not this board - run once without --quick
  exit /b 1
)
findstr /c:"nodelio-p4c5-!VARIANT!.csv" sdkconfig >nul
if errorlevel 1 (
  echo [p4c5-62] sdkconfig partition is not !VARIANT! - run without --quick
  exit /b 1
)
echo [p4c5-62] incremental idf.py build ^(!VARIANT!^; no reconfigure^) ...
idf.py build
if errorlevel 1 exit /b 1
call :sync_ota
exit /b %ERRORLEVEL%

:sync_ota
if /I not "!VARIANT!"=="32m" (
  echo [p4c5-62] !VARIANT! is not the OTA package; skip sila-panel-update.json
  exit /b 0
)
echo [p4c5-62] sync panel OTA manifest + bin to Z-Y ...
python "!REPO!\scripts\nodelio_sync_panel_ota.py" --root "!REPO!" --board sila_p4c5_62
if errorlevel 1 (
  echo [p4c5-62] WARN panel OTA sync failed ^(build ok^)
  exit /b 0
)
exit /b 0
