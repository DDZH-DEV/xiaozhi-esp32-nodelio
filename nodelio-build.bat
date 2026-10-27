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
REM idf.py build 不会吃 config.json 的 sdkconfig_append；补上 WiFi RX；去掉 MultiNet（省 ~4MB PSRAM）
findstr /b /c:"CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16" sdkconfig >nul
if errorlevel 1 (
  echo [nodelio] applying sila WiFi RX buffers to sdkconfig
  powershell -NoProfile -Command ^
    "(Get-Content sdkconfig) -replace 'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=\d+','CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6' -replace 'CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=\d+','CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16' -replace 'CONFIG_ESP_WIFI_RX_BA_WIN=\d+','CONFIG_ESP_WIFI_RX_BA_WIN=6' | Set-Content sdkconfig -Encoding ascii"
)
findstr /b /c:"CONFIG_SR_MN_CN_NONE=y" sdkconfig >nul
if errorlevel 1 (
  echo [nodelio] disabling MultiNet CN ^(no offline local cmd^)
  powershell -NoProfile -Command ^
    "$c=Get-Content sdkconfig -Raw; $c=$c -replace 'CONFIG_SR_MN_CN_MULTINET6_QUANT=y','# CONFIG_SR_MN_CN_MULTINET6_QUANT is not set'; $c=$c -replace '# CONFIG_SR_MN_CN_NONE is not set','CONFIG_SR_MN_CN_NONE=y'; if ($c -notmatch 'CONFIG_SR_MN_CN_NONE=y') { $c=$c + \"`nCONFIG_SR_MN_CN_NONE=y`n\" }; Set-Content sdkconfig -Value $c -Encoding ascii -NoNewline"
)
echo [nodelio] build stamp (code-change day serial) ...
python "%~dp0scripts\nodelio_build_stamp.py" --root "%~dp0."
if errorlevel 1 (
  echo [nodelio] build stamp failed
  exit /b 1
)
echo [nodelio] building SILA-86 ...
idf.py build
if errorlevel 1 exit /b %ERRORLEVEL%

echo [nodelio] sync panel OTA manifest + bin to Z-Y ...
python "%~dp0scripts\nodelio_sync_panel_ota.py" --root "%~dp0."
if errorlevel 1 (
  echo [nodelio] WARN panel OTA sync failed ^(build ok^)
  exit /b 0
)
exit /b 0
