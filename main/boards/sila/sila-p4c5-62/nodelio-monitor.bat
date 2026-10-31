@echo off
setlocal
chcp 65001 >nul

rem SILA P4C5 6.2" — monitor only (USB Serial/JTAG; port may change after reset)
rem   nodelio-monitor.bat
rem   nodelio-monitor.bat COM15

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM8"

set "REPO=D:\xz25"
set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v6.1"
set "IDF_TOOLS_PATH=D:\Espressif"
set "IDF_GITHUB_ASSETS=dl.espressif.com/github_assets"
set "PATH=D:\Espressif\python_env\idf6.1_py3.11_env\Scripts;D:\Espressif\tools\idf-python\3.11.2;%PATH%"
call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
cd /d "%REPO%"

echo [p4c5-62] monitor on %PORT% (no build/flash; esp32p4 / riscv) ...
if exist build\xiaozhi.elf (
  python "%IDF_PATH%\tools\idf_monitor.py" -p %PORT% -b 115200 --toolchain-prefix riscv32-esp-elf- --target esp32p4 build\xiaozhi.elf
) else (
  python "%IDF_PATH%\tools\idf_monitor.py" -p %PORT% -b 115200 --target esp32p4
)
exit /b %ERRORLEVEL%
