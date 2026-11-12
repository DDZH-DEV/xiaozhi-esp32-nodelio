@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul

rem SILA-86 - build then flash+monitor
rem   nodelio-flash.bat              方案 A/R4 常亮背光
rem   nodelio-flash.bat pwm          方案 B 可调背光（IO13 PWM）— 可调板必须用这个
rem   nodelio-flash.bat COM15 pwm
rem   nodelio-flash.bat --nofresh    skip build, flash existing build dir

rem SHIFT 后 %~dp0 可能变空；cwd 又在仓库根，会误找 D:\xz25\nodelio-build.bat
set "SCRIPT_DIR=%~dp0"
set "PORT="
set "NOFRESH=0"
set "BL_ARG="
rem NOTE: do not put SHIFT inside if (...); it does not affect %1 there -> infinite parse loop
:parse
if "%~1"=="" goto :parsed
if /I "%~1"=="--nofresh" set "NOFRESH=1"& shift& goto :parse
if /I "%~1"=="pwm" set "BL_ARG=pwm"& shift& goto :parse
if /I "%~1"=="b" set "BL_ARG=pwm"& shift& goto :parse
if /I "%~1"=="r4" set "BL_ARG=r4"& shift& goto :parse
if /I "%~1"=="a" set "BL_ARG=r4"& shift& goto :parse
set "PORT=%~1"
shift
goto :parse
:parsed

set "REPO=D:\xz25"
cd /d "!REPO!"

if "!NOFRESH!"=="1" goto after_build

call "!SCRIPT_DIR!nodelio-build.bat" !BL_ARG!
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

set "PORT_ARGS="
if not "!PORT!"=="" set "PORT_ARGS=-p !PORT!"
if "!PORT!"=="" (
  echo [sila-86] flash+monitor auto port ...
) else (
  echo [sila-86] flash+monitor on !PORT! ...
)
idf.py !PORT_ARGS! flash monitor
exit /b %ERRORLEVEL%
