@echo off
setlocal
chcp 65001 >nul

rem SILA-86 - monitor only (re-attach after reset / port change)
rem   nodelio-monitor.bat           auto-detect the serial port
rem   nodelio-monitor.bat COM15     force a port

set "PORT=%~1"
set "PORT_ARGS="
if not "%PORT%"=="" set "PORT_ARGS=-p %PORT%"

set "REPO=D:\xz25"
set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v6.1"
set "IDF_TOOLS_PATH=D:\Espressif"
set "IDF_GITHUB_ASSETS=dl.espressif.com/github_assets"
set "PATH=D:\Espressif\python_env\idf6.1_py3.11_env\Scripts;D:\Espressif\tools\idf-python\3.11.2;%PATH%"
call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
cd /d "%REPO%"

if "%PORT%"=="" (
  echo [sila-86] monitor auto port ^(no build/flash; esp32s3^) ...
) else (
  echo [sila-86] monitor on %PORT% ^(no build/flash; esp32s3^) ...
)
if exist build\xiaozhi.elf (
  python "%IDF_PATH%\tools\idf_monitor.py" %PORT_ARGS% -b 115200 --toolchain-prefix xtensa-esp32s3-elf- --target esp32s3 build\xiaozhi.elf
) else (
  python "%IDF_PATH%\tools\idf_monitor.py" %PORT_ARGS% -b 115200 --target esp32s3
)
exit /b %ERRORLEVEL%
