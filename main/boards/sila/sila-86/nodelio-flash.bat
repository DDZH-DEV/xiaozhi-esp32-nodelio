@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul

rem SILA-86 - build then flash+monitor
rem   nodelio-flash.bat              default COM12, build then flash
rem   nodelio-flash.bat COM15        set port
rem   nodelio-flash.bat --nofresh    skip build, flash existing build dir
rem   nodelio-flash.bat COM15 --nofresh

set "PORT=COM12"
set "NOFRESH=0"
rem NOTE: do not put SHIFT inside if (...); it does not affect %1 there -> infinite parse loop
:parse
if "%~1"=="" goto :parsed
if /I "%~1"=="--nofresh" set "NOFRESH=1"& shift& goto :parse
set "PORT=%~1"
shift
goto :parse
:parsed

set "REPO=D:\xz25"
cd /d "!REPO!"

if "!NOFRESH!"=="1" goto after_build

call "%~dp0nodelio-build.bat"
if errorlevel 1 exit /b 1

:after_build
rem build.bat uses setlocal; re-export before flash so PATH sticks
set IDF_PATH=D:\Espressif\frameworks\esp-idf-v6.1
set IDF_TOOLS_PATH=D:\Espressif
set IDF_GITHUB_ASSETS=dl.espressif.com/github_assets
set PATH=D:\Espressif\python_env\idf6.1_py3.11_env\Scripts;D:\Espressif\tools\idf-python\3.11.2;%PATH%
call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
cd /d "!REPO!"

if not exist build\xiaozhi.bin (
  echo [sila-86] missing build\xiaozhi.bin - run nodelio-build.bat first
  exit /b 1
)
findstr /b /c:"CONFIG_BOARD_TYPE_SILA_86=y" sdkconfig >nul
if errorlevel 1 (
  echo [sila-86] CONFIG_BOARD_TYPE_SILA_86 not set - refuse flash ^(wrong board build^)
  exit /b 1
)
echo [sila-86] flash+monitor on !PORT! ...
idf.py -p !PORT! flash monitor
exit /b %ERRORLEVEL%
