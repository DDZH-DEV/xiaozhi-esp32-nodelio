@echo off
setlocal
chcp 65001 >nul
set PORT=%1
if "%PORT%"=="" set PORT=COM12

set IDF_PATH=D:\Espressif\frameworks\esp-idf-v6.1
set IDF_TOOLS_PATH=D:\Espressif
set IDF_GITHUB_ASSETS=dl.espressif.com/github_assets
set PATH=D:\Espressif\python_env\idf6.1_py3.11_env\Scripts;D:\Espressif\tools\idf-python\3.11.2;%PATH%
call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
cd /d D:\xz25
echo [nodelio] monitor on %PORT% (no build/flash) ...
if exist build\xiaozhi.elf (
  python "%IDF_PATH%\tools\idf_monitor.py" -p %PORT% -b 115200 --toolchain-prefix xtensa-esp32s3-elf- --target esp32s3 build\xiaozhi.elf
) else (
  python "%IDF_PATH%\tools\idf_monitor.py" -p %PORT% -b 115200 --target esp32s3
)
exit /b %ERRORLEVEL%
