@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul

rem SILA P4C5 6.2" - P4 host build only (does NOT flash P4 or C5)
rem   nodelio-build.bat          default 32m (W25Q256)
rem   nodelio-build.bat --32m
rem   nodelio-build.bat --16m    Flash>=16MiB board (capacity must be confirmed)
rem
rem Do NOT use sila-86 nodelio-build.bat. This wraps:
rem   python scripts/build.py sila/sila-p4c5-62 --name sila-p4c5-62-32m

set "VARIANT=32m"
:parse
if "%~1"=="" goto :parsed
if /I "%~1"=="--16m" set "VARIANT=16m"& shift& goto :parse
if /I "%~1"=="--32m" set "VARIANT=32m"& shift& goto :parse
echo [p4c5-62] unknown arg: %~1
echo [p4c5-62] usage: nodelio-build.bat [--32m^|--16m]
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

echo [p4c5-62] idf.py --version
idf.py --version
if errorlevel 1 exit /b 1

echo [p4c5-62] build !BOARD_DIR! --name !BUILD_NAME! ...
python scripts\build.py !BOARD_DIR! --name !BUILD_NAME!
exit /b %ERRORLEVEL%
