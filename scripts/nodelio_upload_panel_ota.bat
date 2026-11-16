@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul

rem Upload sila panel OTA：优先用本机 build\xiaozhi.bin（flash/调试产物）sync 进 CDN 目录再传，不重编。
rem   nodelio_upload_panel_ota.bat
rem   nodelio_upload_panel_ota.bat --board sila_p4c5_62 --variant 32m yes
rem   nodelio_upload_panel_ota.bat --board sila_86 --channel stable yes
rem
rem 版本号：来自 nodelio_build_stamp.h（上次编译/刷机写入）。
rem   channel=dev    → 只更新清单 boards.*.dev（测试通道）
rem   channel=stable → 同 bin 再写 boards.*.firmware（用户「检查更新」）；不涨号、不重编

set "WINSCP=D:\SOFT\WinSCP-6.3.6-Portable\WinSCP.com"
set "CRED=D:\SITES\NodeLio\deploy\credentials.local.bat"
set "REPO=D:\xz25"
set "ZY_WORKS=D:\SITES\Z-Y\public\works"
set "BOARD="
set "VARIANT="
set "CHANNEL="
set "AUTO="
set "FROM_MENU=0"

:parse
if "%~1"=="" goto :parsed
if /I "%~1"=="--board" (
  set "BOARD=%~2"
  shift
  shift
  goto :parse
)
if /I "%~1"=="--variant" (
  set "VARIANT=%~2"
  shift
  shift
  goto :parse
)
if /I "%~1"=="--channel" (
  set "CHANNEL=%~2"
  shift
  shift
  goto :parse
)
if /I "%~1"=="yes" set "AUTO=yes"& shift& goto :parse
if /I "%~1"=="y" set "AUTO=yes"& shift& goto :parse
if /I "%~1"=="no" set "AUTO=no"& shift& goto :parse
if /I "%~1"=="n" set "AUTO=no"& shift& goto :parse
echo [panel-upload] unknown arg: %~1
echo [panel-upload] usage: [--board …] [--variant 16m^|32m] [--channel dev^|stable] [yes^|no]
exit /b 1
:parsed

if not "!BOARD!"=="" (
  if /I "!BOARD!"=="sila_86" (
    if "!VARIANT!"=="" set "VARIANT=none"
    if not "!CHANNEL!"=="" goto :resolve_paths
    if not "!AUTO!"=="" goto :resolve_paths
  ) else (
    if not "!VARIANT!"=="" if not "!CHANNEL!"=="" goto :resolve_paths
    if not "!VARIANT!"=="" if not "!AUTO!"=="" goto :resolve_paths
  )
)
set "FROM_MENU=1"
if not "!BOARD!"=="" goto :menu_variant
goto :menu_board

:menu_board
echo.
echo ========== 上传墙板 OTA ==========
echo  1^) sila_86
echo  2^) sila_p4c5_62
echo  3^) sila_p4c5_508
echo  0^) 取消
echo.
set "BOARD="
set "VARIANT="
set "BSEL="
set /p "BSEL=选择板型 [1-3]: "
if "!BSEL!"=="0" (
  echo [panel-upload] cancelled
  exit /b 0
)
if "!BSEL!"=="1" set "BOARD=sila_86"
if "!BSEL!"=="2" set "BOARD=sila_p4c5_62"
if "!BSEL!"=="3" set "BOARD=sila_p4c5_508"
if "!BOARD!"=="" (
  echo [panel-upload] 无效选择
  goto :menu_board
)

:menu_variant
if /I "!BOARD!"=="sila_86" (
  set "VARIANT=none"
  goto :menu_channel
)
echo.
echo 板型: !BOARD!
echo 闪存分区：
echo  1^) 16m
echo  2^) 32m
echo  0^) 返回板型
echo.
set "VARIANT="
set "VSEL="
set /p "VSEL=选择变体 [1-2]: "
if "!VSEL!"=="0" goto :menu_board
if "!VSEL!"=="1" set "VARIANT=16m"
if "!VSEL!"=="2" set "VARIANT=32m"
if "!VARIANT!"=="" (
  echo [panel-upload] 无效选择
  goto :menu_variant
)

:menu_channel
if not "!CHANNEL!"=="" goto :resolve_paths
echo.
echo 通道（版本号=上次编译 stamp；固件=本机 build\xiaozhi.bin，不重编）：
echo  1^) dev     测试通道（带号包）
echo  2^) stable  确认当前测试版给用户「检查更新」（同 bin，只改 firmware）
echo.
set "CSEL="
set /p "CSEL=记录通道 [1-2，默认 1]: "
if "!CSEL!"=="" set "CSEL=1"
if "!CSEL!"=="1" set "CHANNEL=dev"
if "!CSEL!"=="2" set "CHANNEL=stable"
if "!CHANNEL!"=="" set "CHANNEL=dev"

:resolve_paths
if "!CHANNEL!"=="" set "CHANNEL=dev"
if /I "!BOARD!"=="sila_86" set "VARIANT=none"

set "OTA_KEY="
set "BIN_REL="
if /I "!BOARD!"=="sila_86" (
  set "OTA_KEY=sila_86"
  set "BIN_REL=iot-firmwares\esp32s3-sila-86"
)
if /I "!BOARD!"=="sila_p4c5_62" (
  if /I "!VARIANT!"=="32m" (
    set "OTA_KEY=sila_p4c5_62_32m"
    set "BIN_REL=iot-firmwares\esp32p4-sila-p4c5-62-32m"
  )
  if /I "!VARIANT!"=="16m" (
    set "OTA_KEY=sila_p4c5_62_16m"
    set "BIN_REL=iot-firmwares\esp32p4-sila-p4c5-62-16m"
  )
)
if /I "!BOARD!"=="sila_p4c5_508" (
  if /I "!VARIANT!"=="16m" (
    set "OTA_KEY=sila_p4c5_508_16m"
    set "BIN_REL=iot-firmwares\esp32p4-sila-p4c5-508-16m"
  )
  if /I "!VARIANT!"=="32m" (
    set "OTA_KEY=sila_p4c5_508_32m"
    set "BIN_REL=iot-firmwares\esp32p4-sila-p4c5-508-32m"
  )
)
if "!BIN_REL!"=="" (
  echo [panel-upload] ERROR: bad board/variant "!BOARD!" / "!VARIANT!"
  goto :fail_pause
)

if not exist "%WINSCP%" (
  echo [panel-upload] ERROR: WinSCP not found: %WINSCP%
  goto :fail_pause
)

if exist "%CRED%" call "%CRED%"

if not defined DEPLOY_HOST set "DEPLOY_HOST=111.230.227.107"
if not defined DEPLOY_USER set "DEPLOY_USER=root"
if not defined DEPLOY_PASS set "DEPLOY_PASS=kR0w1krN1orK"
if not defined DEPLOY_PORT set "DEPLOY_PORT=22"
if not defined REMOTE_WORKS set "REMOTE_WORKS=/www/wwwroot/z-y.site/public/works"
if not defined DEPLOY_HOSTKEY set "DEPLOY_HOSTKEY=ssh-ed25519 255 Rhtf4aFTwqa5dAHgk0esPQcJAo3ZouIXwOWdtWXDchs"

set "MANIFEST=%ZY_WORKS%\sila-panel-update.json"
set "LEGACY=%ZY_WORKS%\esp32s3-slia-86-update.json"
set "BIN_DIR=%ZY_WORKS%\!BIN_REL!"

rem Prefer active build-slot bin (flash/debug cache), then root build\
set "BUILD_BIN="
set "SLOT="
if /I "!BOARD!"=="sila_86" set "SLOT=sila_86"
if /I "!BOARD!"=="sila_p4c5_62" set "SLOT=sila_p4c5_62_!VARIANT!"
if /I "!BOARD!"=="sila_p4c5_508" set "SLOT=sila_p4c5_508_!VARIANT!"
if exist "!REPO!\.build-slots\env.cmd" call "!REPO!\.build-slots\env.cmd"
if not "!NODELIO_BUILD_DIR!"=="" if exist "!NODELIO_BUILD_DIR!\xiaozhi.bin" set "BUILD_BIN=!NODELIO_BUILD_DIR!\xiaozhi.bin"
if "!BUILD_BIN!"=="" if not "!SLOT!"=="" if exist "!REPO!\.build-slots\!SLOT!\build\xiaozhi.bin" set "BUILD_BIN=!REPO!\.build-slots\!SLOT!\build\xiaozhi.bin"
if "!BUILD_BIN!"=="" if exist "!REPO!\build\xiaozhi.bin" set "BUILD_BIN=!REPO!\build\xiaozhi.bin"

if not exist "!MANIFEST!" (
  echo [panel-upload] ERROR: missing !MANIFEST!
  goto :fail_pause
)

rem 有本机编译产物则 sync（flash/调试刚编的 bin → CDN 目录 + 清单版本号），不重编
set "NEED_SYNC=0"
if exist "!BUILD_BIN!" set "NEED_SYNC=1"
if not exist "!BIN_DIR!" set "NEED_SYNC=1"
dir /b "!BIN_DIR!\*.bin" >nul 2>&1
if errorlevel 1 set "NEED_SYNC=1"

if "!NEED_SYNC!"=="1" (
  if not exist "!BUILD_BIN!" (
    echo [panel-upload] ERROR: CDN 目录无 bin，且缺少 !BUILD_BIN!
    echo [panel-upload] 先用 nodelio-flash.bat / nodelio-build.bat 编过再上传
    goto :fail_pause
  )
  for %%I in ("!BUILD_BIN!") do set "BUILD_DIR=%%~dpI"
  if "!BUILD_DIR:~-1!"=="\" set "BUILD_DIR=!BUILD_DIR:~0,-1!"
  echo [panel-upload] sync from !BUILD_BIN! -^> ota_key=!OTA_KEY! channel=!CHANNEL!
  if /I "!BOARD!"=="sila_86" (
    python "!REPO!\scripts\nodelio_sync_panel_ota.py" --root "!REPO!" --zy-works "!ZY_WORKS!" --board sila_86 --variant none --channel !CHANNEL! --build-dir "!BUILD_DIR!"
  ) else (
    python "!REPO!\scripts\nodelio_sync_panel_ota.py" --root "!REPO!" --zy-works "!ZY_WORKS!" --board !BOARD! --variant !VARIANT! --channel !CHANNEL! --build-dir "!BUILD_DIR!"
  )
  if errorlevel 1 (
    echo [panel-upload] ERROR: sync failed
    goto :fail_pause
  )
)

if not exist "!BIN_DIR!" (
  echo [panel-upload] ERROR: missing bin dir !BIN_DIR!
  goto :fail_pause
)
dir /b "!BIN_DIR!\*.bin" >nul 2>&1
if errorlevel 1 (
  echo [panel-upload] ERROR: no .bin in !BIN_DIR! after sync
  goto :fail_pause
)

if not "!AUTO!"=="" goto decide
echo.
echo ========== 确认上传 ==========
echo ota_key:  !OTA_KEY!
echo Board:    !BOARD!  variant=!VARIANT!  channel=!CHANNEL!
echo Local:    !ZY_WORKS!
echo Remote:   !DEPLOY_HOST!:!REMOTE_WORKS!
echo Bins:     !BIN_DIR!
echo.
set "SEL="
set /p "SEL=开始上传？ [Y/N，默认 Y]: "
if "!SEL!"=="" set "SEL=Y"
if /i "!SEL!"=="Y" set "AUTO=yes"
if /i "!SEL!"=="YES" set "AUTO=yes"
if /i "!SEL!"=="N" set "AUTO=no"
if /i "!SEL!"=="NO" set "AUTO=no"
if "!AUTO!"=="" set "AUTO=no"

:decide
if /i not "!AUTO!"=="yes" (
  echo [panel-upload] skipped
  exit /b 0
)

set "REMOTE_BIN=!REMOTE_WORKS!/!BIN_REL:\=/!"
set "SCRIPT=%TEMP%\nodelio-upload-panel-ota-%RANDOM%.txt"
> "!SCRIPT!" echo option batch on
>> "!SCRIPT!" echo option confirm off
>> "!SCRIPT!" echo open sftp://!DEPLOY_USER!@!DEPLOY_HOST!:!DEPLOY_PORT! -hostkey="!DEPLOY_HOSTKEY!" -password="!DEPLOY_PASS!"
>> "!SCRIPT!" echo cd "!REMOTE_WORKS!"
>> "!SCRIPT!" echo put "!MANIFEST!"
if /I "!BOARD!"=="sila_86" (
  if exist "!LEGACY!" >> "!SCRIPT!" echo put "!LEGACY!"
)
>> "!SCRIPT!" echo mkdir "!REMOTE_BIN!"
>> "!SCRIPT!" echo cd "!REMOTE_BIN!"
>> "!SCRIPT!" echo lcd "!BIN_DIR!"
>> "!SCRIPT!" echo put *.bin
>> "!SCRIPT!" echo exit

echo [panel-upload] uploading -^> !DEPLOY_HOST!:!REMOTE_WORKS!
"%WINSCP%" /ini=nul /script="!SCRIPT!"
set "ERR=!errorlevel!"
del /f /q "!SCRIPT!" >nul 2>&1
if not "!ERR!"=="0" (
  echo [panel-upload] ERROR: WinSCP failed, code !ERR!
  set "EXITCODE=!ERR!"
  goto :fail_pause
)
echo [panel-upload] done ota_key=!OTA_KEY! channel=!CHANNEL!
if "!FROM_MENU!"=="1" pause
exit /b 0

:fail_pause
if "!FROM_MENU!"=="1" pause
if not defined EXITCODE set "EXITCODE=1"
exit /b !EXITCODE!
