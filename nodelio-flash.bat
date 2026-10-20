@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul

rem 常用入口：构建号 → 编译 → 烧录+监视
rem   nodelio-flash.bat              默认 COM12，先编再烧
rem   nodelio-flash.bat COM15        指定口
rem   nodelio-flash.bat --nofresh    不重编，只烧已有 build\
rem   nodelio-flash.bat COM15 --nofresh

set "PORT=COM12"
set "NOFRESH=0"
:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--nofresh" (
  set "NOFRESH=1"
  shift
  goto parse
)
set "PORT=%~1"
shift
goto parse
:parsed

cd /d D:\xz25

if "!NOFRESH!"=="1" goto after_build

call "%~dp0nodelio-build.bat"
if errorlevel 1 exit /b 1

:after_build
rem build.bat 用了 setlocal，export 的 PATH 不会留到本脚本，烧录前必须重新 export
set IDF_PATH=D:\Espressif\frameworks\esp-idf-v6.1
set IDF_TOOLS_PATH=D:\Espressif
set IDF_GITHUB_ASSETS=dl.espressif.com/github_assets
set PATH=D:\Espressif\python_env\idf6.1_py3.11_env\Scripts;D:\Espressif\tools\idf-python\3.11.2;%PATH%
call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
cd /d D:\xz25

if not exist build\xiaozhi.bin (
  echo [nodelio] missing build\xiaozhi.bin - run nodelio-build.bat first
  exit /b 1
)
echo [nodelio] flash+monitor on !PORT! ...
idf.py -p !PORT! flash monitor
exit /b %ERRORLEVEL%
