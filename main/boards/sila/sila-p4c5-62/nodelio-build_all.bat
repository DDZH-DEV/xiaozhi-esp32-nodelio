@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul
rem SILA P4C5 6.2" 完整版：编译 + merge-bin 整片镜像（bootloader/分区/ota_data/app/assets）
rem   nodelio-build_all.bat          默认 32m（W25Q256）
rem   nodelio-build_all.bat --32m
rem   nodelio-build_all.bat --16m
rem 产物：
rem   build\xiaozhi.bin
rem   build\generated_assets.bin
rem   build\merged-binary.bin   ← 从 0x0 起的整片，可直接烧
rem 仅改应用、日常迭代用 nodelio-build.bat；出厂/整片交付用本脚本。
rem 勿套用 sila-86 的 nodelio-build_all.bat（目标芯片、分区、ES8389 都不同）。

set "VARIANT=32m"
:parse
if "%~1"=="" goto :parsed
if /I "%~1"=="--16m" set "VARIANT=16m"& shift& goto :parse
if /I "%~1"=="--32m" set "VARIANT=32m"& shift& goto :parse
echo [p4c5-62] unknown arg: %~1
echo [p4c5-62] usage: nodelio-build_all.bat [--32m^|--16m]
exit /b 1
:parsed

set "BOARD_DIR=sila/sila-p4c5-62"
set "BUILD_NAME=sila-p4c5-62-!VARIANT!"

set "REPO=D:\xz25"
set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v6.1"
set "IDF_TOOLS_PATH=D:\Espressif"
set "IDF_GITHUB_ASSETS=dl.espressif.com/github_assets"
set "PATH=D:\Espressif\python_env\idf6.1_py3.11_env\Scripts;D:\Espressif\tools\idf-python\3.11.2;%PATH%"
call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
cd /d "!REPO!"
call "!REPO!\scripts\nodelio_activate_build_slot.bat" sila_p4c5_62_!VARIANT!
if errorlevel 1 exit /b 1
call "!REPO!\.build-slots\env.cmd"
python "!REPO!\scripts\nodelio_patch_esp_ml307.py"
if errorlevel 1 exit /b 1

echo [p4c5-62] build stamp ^(sila_p4c5_62 variant=!VARIANT!^) ...
python "!REPO!\scripts\nodelio_build_stamp.py" --root "!REPO!" --board sila_p4c5_62 --variant !VARIANT!
if errorlevel 1 (
  echo [p4c5-62] build stamp failed
  exit /b 1
)

set "CFG=!NODELIO_SDKCONFIG!"
if not exist "!CFG!" set "CFG=sdkconfig"
set "NEED_FULL=1"
if exist "!CFG!" (
  findstr /b /c:"CONFIG_BOARD_TYPE_SILA_P4C5_62=y" "!CFG!" >nul
  if not errorlevel 1 (
    findstr /c:"nodelio-p4c5-!VARIANT!.csv" "!CFG!" >nul
    if not errorlevel 1 set "NEED_FULL=0"
  )
)

if "!NEED_FULL!"=="1" (
  echo [p4c5-62] sdkconfig is not !BUILD_NAME!. Reconfigure esp32p4 via build.py.
  echo [p4c5-62] previous panel cache kept under .build-slots\; result copied into this slot.
  python scripts\build.py !BOARD_DIR! --name !BUILD_NAME!
  if errorlevel 1 exit /b 1
  if not "!NODELIO_BUILD_DIR!"=="" robocopy "build" "!NODELIO_BUILD_DIR!" /E /COPY:DAT /R:1 /W:1 /NFL /NDL /NJH /NJS >nul
  if exist "sdkconfig" if not "!NODELIO_SDKCONFIG!"=="" copy /Y "sdkconfig" "!NODELIO_SDKCONFIG!" >nul
  goto verify
)

echo [p4c5-62] building !BUILD_NAME! ^(slot^) ...
call "!REPO!\scripts\nodelio_idf.cmd" build
if errorlevel 1 exit /b %ERRORLEVEL%

echo [p4c5-62] merge-bin full flash image ...
call "!REPO!\scripts\nodelio_idf.cmd" merge-bin
if errorlevel 1 exit /b %ERRORLEVEL%

:verify
set "BD=build"
if not "!NODELIO_BUILD_DIR!"=="" set "BD=!NODELIO_BUILD_DIR!"
if not exist "!BD!\xiaozhi.bin" (
  echo [p4c5-62] missing !BD!\xiaozhi.bin
  exit /b 1
)
if not exist "!BD!\generated_assets.bin" (
  echo [p4c5-62] missing !BD!\generated_assets.bin
  exit /b 1
)
if not exist "!BD!\flash_args" (
  echo [p4c5-62] missing !BD!\flash_args
  exit /b 1
)
if not exist "!BD!\merged-binary.bin" (
  echo [p4c5-62] missing !BD!\merged-binary.bin after merge-bin
  exit /b 1
)

for %%A in ("!BD!\merged-binary.bin") do echo [p4c5-62] OK merged-binary.bin %%~zA bytes
for %%A in ("!BD!\xiaozhi.bin") do echo [p4c5-62]    xiaozhi.bin %%~zA bytes
for %%A in ("!BD!\generated_assets.bin") do echo [p4c5-62]    generated_assets.bin %%~zA bytes

echo [p4c5-62] sync panel OTA manifest + bin to Z-Y ...
python "!REPO!\scripts\nodelio_sync_panel_ota.py" --root "!REPO!" --board sila_p4c5_62 --variant !VARIANT! --channel dev --build-dir "!BD!"
if errorlevel 1 (
  echo [p4c5-62] WARN panel OTA sync failed ^(build+merge ok^)
  exit /b 0
)
exit /b 0
