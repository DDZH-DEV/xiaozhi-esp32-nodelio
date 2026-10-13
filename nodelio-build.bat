@echo off
setlocal
chcp 65001 >nul
set IDF_PATH=D:\Espressif\frameworks\esp-idf-v6.1
set IDF_TOOLS_PATH=D:\Espressif
set IDF_GITHUB_ASSETS=dl.espressif.com/github_assets
set PATH=D:\Espressif\python_env\idf6.1_py3.11_env\Scripts;D:\Espressif\tools\idf-python\3.11.2;%PATH%
call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
cd /d D:\xz25
if not exist sdkconfig (
  echo [nodelio] no sdkconfig - run idf.py set-target esp32s3
  exit /b 1
)
findstr /b /c:"CONFIG_BOARD_TYPE_SILA_86=y" sdkconfig >nul
if errorlevel 1 (
  echo [nodelio] CONFIG_BOARD_TYPE_SILA_86 not set
  exit /b 1
)
echo [nodelio] building SILA-86 ...
idf.py build
exit /b %ERRORLEVEL%
