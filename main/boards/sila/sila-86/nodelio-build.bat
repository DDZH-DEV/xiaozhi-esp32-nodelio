@echo off
setlocal
chcp 65001 >nul
rem SILA-86 — build only

set "REPO=D:\xz25"
set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v6.1"
set "IDF_TOOLS_PATH=D:\Espressif"
set "IDF_GITHUB_ASSETS=dl.espressif.com/github_assets"
set "PATH=D:\Espressif\python_env\idf6.1_py3.11_env\Scripts;D:\Espressif\tools\idf-python\3.11.2;%PATH%"
call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
cd /d "%REPO%"
if not exist sdkconfig (
  echo [sila-86] no sdkconfig - run idf.py set-target esp32s3
  exit /b 1
)
findstr /b /c:"CONFIG_BOARD_TYPE_SILA_86=y" sdkconfig >nul
if errorlevel 1 (
  echo [sila-86] CONFIG_BOARD_TYPE_SILA_86 not set
  exit /b 1
)
REM idf.py build ignores config.json sdkconfig_append; patch WiFi RX; disable MultiNet (~4MB PSRAM)
findstr /b /c:"CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16" sdkconfig >nul
if errorlevel 1 (
  echo [sila-86] applying sila WiFi RX buffers to sdkconfig
  powershell -NoProfile -Command ^
    "(Get-Content sdkconfig) -replace 'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=\d+','CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6' -replace 'CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=\d+','CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16' -replace 'CONFIG_ESP_WIFI_RX_BA_WIN=\d+','CONFIG_ESP_WIFI_RX_BA_WIN=6' | Set-Content sdkconfig -Encoding ascii"
)
findstr /b /c:"CONFIG_SR_MN_CN_NONE=y" sdkconfig >nul
if errorlevel 1 (
  echo [sila-86] disabling MultiNet CN ^(no offline local cmd^)
  powershell -NoProfile -Command ^
    "$c=Get-Content sdkconfig -Raw; $c=$c -replace 'CONFIG_SR_MN_CN_MULTINET6_QUANT=y','# CONFIG_SR_MN_CN_MULTINET6_QUANT is not set'; $c=$c -replace '# CONFIG_SR_MN_CN_NONE is not set','CONFIG_SR_MN_CN_NONE=y'; if ($c -notmatch 'CONFIG_SR_MN_CN_NONE=y') { $c=$c + \"`nCONFIG_SR_MN_CN_NONE=y`n\" }; Set-Content sdkconfig -Value $c -Encoding ascii -NoNewline"
)
REM Existing sdkconfig overrides defaults. Use software AES: PSRAM TLS buffers
REM otherwise need DMA bounce buffers after AFE has exhausted internal SRAM.
findstr /x /c:"# CONFIG_MBEDTLS_HARDWARE_AES is not set" sdkconfig >nul
if errorlevel 1 (
  echo [sila-86] disabling hardware AES DMA for low-SRAM voice operation
  powershell -NoProfile -Command ^
    "$c=Get-Content sdkconfig -Raw; $c=$c -replace '(?m)^CONFIG_MBEDTLS_HARDWARE_AES=.*\r?\n?',''; $c=$c.TrimEnd()+[Environment]::NewLine+'# CONFIG_MBEDTLS_HARDWARE_AES is not set'+[Environment]::NewLine; Set-Content sdkconfig -Value $c -Encoding ascii -NoNewline"
  if errorlevel 1 exit /b 1
)
echo [sila-86] build stamp (code-change day serial) ...
python "%REPO%\scripts\nodelio_build_stamp.py" --root "%REPO%"
if errorlevel 1 (
  echo [sila-86] build stamp failed
  exit /b 1
)
echo [sila-86] building ...
idf.py build
if errorlevel 1 exit /b %ERRORLEVEL%

echo [sila-86] sync panel OTA manifest + bin to Z-Y ...
python "%REPO%\scripts\nodelio_sync_panel_ota.py" --root "%REPO%"
if errorlevel 1 (
  echo [sila-86] WARN panel OTA sync failed ^(build ok^)
  exit /b 0
)
exit /b 0
