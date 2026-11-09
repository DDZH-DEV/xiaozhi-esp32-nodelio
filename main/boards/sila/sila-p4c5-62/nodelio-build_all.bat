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
python "!REPO!\scripts\nodelio_patch_esp_ml307.py"
if errorlevel 1 exit /b 1

echo [p4c5-62] build stamp ^(sila_p4c5_62 day serial^) ...
python "!REPO!\scripts\nodelio_build_stamp.py" --root "!REPO!" --board sila_p4c5_62
if errorlevel 1 (
  echo [p4c5-62] build stamp failed
  exit /b 1
)

rem 86 与本板共用仓库 sdkconfig。上次编过 86 或另一种 Flash 时必须走 build.py。
set "NEED_FULL=1"
if exist sdkconfig (
  findstr /b /c:"CONFIG_BOARD_TYPE_SILA_P4C5_62=y" sdkconfig >nul
  if not errorlevel 1 (
    findstr /c:"nodelio-p4c5-!VARIANT!.csv" sdkconfig >nul
    if not errorlevel 1 set "NEED_FULL=0"
  )
)

if "!NEED_FULL!"=="1" (
  echo [p4c5-62] sdkconfig is not !BUILD_NAME!. Reconfigure esp32p4 via build.py.
  echo [p4c5-62] this fullcleans the current build directory. build.py already merge-bin.
  python scripts\build.py !BOARD_DIR! --name !BUILD_NAME!
  if errorlevel 1 exit /b 1
  goto verify
)

echo [p4c5-62] building !BUILD_NAME! ...
idf.py build
if errorlevel 1 exit /b %ERRORLEVEL%

echo [p4c5-62] merge-bin full flash image ...
idf.py merge-bin
if errorlevel 1 exit /b %ERRORLEVEL%

:verify
if not exist build\xiaozhi.bin (
  echo [p4c5-62] missing build\xiaozhi.bin
  exit /b 1
)
if not exist build\generated_assets.bin (
  echo [p4c5-62] missing build\generated_assets.bin
  exit /b 1
)
if not exist build\flash_args (
  echo [p4c5-62] missing build\flash_args
  exit /b 1
)
if not exist build\merged-binary.bin (
  echo [p4c5-62] missing build\merged-binary.bin after merge-bin
  exit /b 1
)

for %%A in (build\merged-binary.bin) do echo [p4c5-62] OK merged-binary.bin %%~zA bytes
for %%A in (build\xiaozhi.bin) do echo [p4c5-62]    xiaozhi.bin %%~zA bytes
for %%A in (build\generated_assets.bin) do echo [p4c5-62]    generated_assets.bin %%~zA bytes

if /I not "!VARIANT!"=="32m" (
  echo [p4c5-62] !VARIANT! is not the OTA package; skip sila-panel-update.json
  exit /b 0
)
echo [p4c5-62] sync panel OTA manifest + bin to Z-Y ...
python "!REPO!\scripts\nodelio_sync_panel_ota.py" --root "!REPO!" --board sila_p4c5_62
if errorlevel 1 (
  echo [p4c5-62] WARN panel OTA sync failed ^(build+merge ok^)
  exit /b 0
)
exit /b 0
