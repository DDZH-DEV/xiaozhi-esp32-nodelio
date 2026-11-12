@echo off
setlocal
chcp 65001 >nul
rem SILA-86 - build only
rem   nodelio-build.bat          方案 A / R4：SY7200 常亮（FIXED_BACKLIGHT=y）
rem   nodelio-build.bat pwm      方案 B / IP5306+RY3730：IO13 PWM 可调背光
rem 可调背光板刷了 R4 固件会全黑（NoopBacklight 不推 IO13）。

set "BL_MODE=r4"
if /I "%~1"=="pwm" set "BL_MODE=pwm"
if /I "%~1"=="b" set "BL_MODE=pwm"
if /I "%~1"=="r4" set "BL_MODE=r4"
if /I "%~1"=="a" set "BL_MODE=r4"

set "REPO=D:\xz25"
set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v6.1"
set "IDF_TOOLS_PATH=D:\Espressif"
set "IDF_GITHUB_ASSETS=dl.espressif.com/github_assets"
set "PATH=D:\Espressif\python_env\idf6.1_py3.11_env\Scripts;D:\Espressif\tools\idf-python\3.11.2;%PATH%"
call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
cd /d "%REPO%"
python "%REPO%\scripts\nodelio_patch_esp_ml307.py"
if errorlevel 1 exit /b 1
rem P4 and 86 share this tree's sdkconfig. Last P4 build leaves esp32p4 selected.
findstr /b /c:"CONFIG_BOARD_TYPE_SILA_86=y" sdkconfig >nul
if errorlevel 1 (
  echo [sila-86] sdkconfig is not SILA_86. Reconfigure esp32s3 via build.py.
  echo [sila-86] this fullcleans the current build directory.
  if /I "%BL_MODE%"=="pwm" (
    python scripts\build.py sila/sila-86 --name sila-86-pwm
  ) else (
    python scripts\build.py sila/sila-86 --name sila-86
  )
  if errorlevel 1 exit /b 1
  echo [sila-86] sync panel OTA manifest + bin to Z-Y ...
  python "%REPO%\scripts\nodelio_sync_panel_ota.py" --root "%REPO%"
  if errorlevel 1 (
    echo [sila-86] WARN panel OTA sync failed ^(build ok^)
    exit /b 0
  )
  exit /b 0
)
echo [sila-86] backlight mode=%BL_MODE%
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
if /I "%BL_MODE%"=="pwm" (
  rem 方案 B：必须关掉 FIXED，否则 PwmBacklight 不编进、IO13 不亮 → 黑屏
  findstr /b /c:"# CONFIG_NODELIO_SILA86_FIXED_BACKLIGHT is not set" sdkconfig >nul
  if errorlevel 1 (
    echo [sila-86] disabling FIXED_BACKLIGHT ^(scheme B IO13 PWM^)
    powershell -NoProfile -Command ^
      "$c=Get-Content sdkconfig -Raw; $c=$c -replace '(?m)^CONFIG_NODELIO_SILA86_FIXED_BACKLIGHT=y\r?\n?',''; $c=$c -replace '(?m)^# CONFIG_NODELIO_SILA86_FIXED_BACKLIGHT is not set\r?\n?',''; $c=$c.TrimEnd()+[Environment]::NewLine+'# CONFIG_NODELIO_SILA86_FIXED_BACKLIGHT is not set'+[Environment]::NewLine; Set-Content sdkconfig -Value $c -Encoding ascii -NoNewline"
  ) else (
    echo [sila-86] FIXED_BACKLIGHT already off ^(scheme B^)
  )
) else (
  rem 方案 A / R4：SY7200 常亮；IO13=KEY，禁止 LEDC
  findstr /b /c:"CONFIG_NODELIO_SILA86_FIXED_BACKLIGHT=y" sdkconfig >nul
  if errorlevel 1 (
    echo [sila-86] enabling FIXED_BACKLIGHT ^(R4 SY7200 always-on^)
    powershell -NoProfile -Command ^
      "$c=Get-Content sdkconfig -Raw; $c=$c -replace '(?m)^# CONFIG_NODELIO_SILA86_FIXED_BACKLIGHT is not set\r?\n?',''; if ($c -notmatch 'CONFIG_NODELIO_SILA86_FIXED_BACKLIGHT=y') { $c=$c.TrimEnd()+[Environment]::NewLine+'CONFIG_NODELIO_SILA86_FIXED_BACKLIGHT=y'+[Environment]::NewLine }; Set-Content sdkconfig -Value $c -Encoding ascii -NoNewline"
  )
)
REM Offload WiFi/LwIP, .ext_ram.bss, and >64B mallocs to 8MB PSRAM to preserve internal DMA SRAM
findstr /b /c:"CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y" sdkconfig >nul
if errorlevel 1 (
  echo [sila-86] enabling PSRAM offload for WiFi/LwIP, .ext_ram.bss, and malloc threshold
  powershell -NoProfile -Command ^
    "$c=Get-Content sdkconfig -Raw; $c=$c -replace '# CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP is not set','CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y'; $c=$c -replace '# CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is not set','CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y'; $c=$c -replace 'CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=\d+','CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=64'; $c=$c -replace 'CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=\d+','CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768'; Set-Content sdkconfig -Value $c -Encoding ascii -NoNewline"
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
