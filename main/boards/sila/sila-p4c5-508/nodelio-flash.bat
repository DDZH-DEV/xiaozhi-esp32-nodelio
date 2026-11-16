@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul

rem SILA P4C5 5.08" - P4 host flash only (does NOT program C5 slave)
rem   本板 Flash=16MiB，默认 --16m（partitions/nodelio-p4c5-508-16m.csv）
rem   nodelio-flash.bat                 auto port, default 16m; incremental if sdkconfig matches
rem   nodelio-flash.bat COM15           force a port
rem   nodelio-flash.bat --16m           16m（本板默认）
rem   nodelio-flash.bat --32m           仅当确认是 32MiB 颗粒时
rem   nodelio-flash.bat --nofresh       skip build, flash existing build/
rem   nodelio-flash.bat --rebuild       full scripts/build.py reconfigure
rem   nodelio-flash.bat --quick         incremental build, app-flash only + monitor
rem   nodelio-flash.bat COM15 --16m --nofresh
rem
rem Do NOT use sila-86 / sila-p4c5-62 bats. Full flash via flash_args.
rem C5 slave: nodelio-flash-c5.bat COMx  (U14 UART; separate from this script)

set "PORT="
set "NOFRESH=0"
set "QUICK=0"
set "REBUILD=0"
set "VARIANT=16m"
rem NOTE: do not put SHIFT inside if (...); it does not affect %1 there -> infinite parse loop
:parse
if "%~1"=="" goto :parsed
if /I "%~1"=="--nofresh" set "NOFRESH=1"& shift& goto :parse
if /I "%~1"=="--rebuild" set "REBUILD=1"& shift& goto :parse
if /I "%~1"=="--quick" set "QUICK=1"& shift& goto :parse
if /I "%~1"=="--16m" set "VARIANT=16m"& shift& goto :parse
if /I "%~1"=="--32m" set "VARIANT=32m"& shift& goto :parse
set "PORT=%~1"
shift
goto :parse
:parsed

set "BOARD_DIR=sila/sila-p4c5-508"
set "BUILD_NAME=sila-p4c5-508-!VARIANT!"
if /I "!VARIANT!"=="16m" (
  set "PART_CSV=partitions/nodelio-p4c5-508-16m.csv"
) else (
  set "PART_CSV=partitions/nodelio-p4c5-!VARIANT!.csv"
)

set "REPO=D:\xz25"
set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v6.1"
set "IDF_TOOLS_PATH=D:\Espressif"
set "IDF_GITHUB_ASSETS=dl.espressif.com/github_assets"
set "PATH=D:\Espressif\python_env\idf6.1_py3.11_env\Scripts;D:\Espressif\tools\idf-python\3.11.2;%PATH%"

call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
cd /d "!REPO!"

rem Per-board object cache under .build-slots\
call "!REPO!\scripts\nodelio_activate_build_slot.bat" sila_p4c5_508_!VARIANT!
if errorlevel 1 exit /b 1
call "!REPO!\.build-slots\env.cmd"
if "!NODELIO_BUILD_DIR!"=="" (
  echo [p4c5-508] ERROR: NODELIO_BUILD_DIR unset after activate
  exit /b 1
)

if "!NOFRESH!"=="1" goto after_build
if "!QUICK!"=="1" goto quick_build

set "NEED_FULL=1"
if "!REBUILD!"=="1" goto do_full_build
if not exist "!NODELIO_SDKCONFIG!" goto do_full_build
if not exist "!NODELIO_BUILD_DIR!\flash_args" goto do_full_build
findstr /b /c:"CONFIG_BOARD_TYPE_SILA_P4C5_508=y" "!NODELIO_SDKCONFIG!" >nul
if errorlevel 1 goto do_full_build
findstr /c:"!PART_CSV!" "!NODELIO_SDKCONFIG!" >nul
if errorlevel 1 goto do_full_build
set "NEED_FULL=0"
goto do_incremental

:do_full_build
if "!NEED_FULL!"=="0" goto do_incremental
goto run_full_build

:do_incremental
echo [p4c5-508] sdkconfig already !BUILD_NAME! - incremental slot build
echo [p4c5-508] pass --rebuild to reconfigure, --nofresh to skip compile
call "!REPO!\scripts\nodelio_idf.cmd" build
if errorlevel 1 exit /b 1
goto after_build

:run_full_build

echo [p4c5-508] idf.py --version
idf.py --version
if errorlevel 1 exit /b 1

echo [p4c5-508] full reconfigure !BOARD_DIR! --name !BUILD_NAME! ...
echo [p4c5-508] NOTE: build.py still uses root build\; result is copied into the slot
python scripts\build.py !BOARD_DIR! --name !BUILD_NAME!
if errorlevel 1 exit /b 1

call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
cd /d "!REPO!"
call :import_root_build_to_slot
call "!REPO!\scripts\nodelio_activate_build_slot.bat" sila_p4c5_508_!VARIANT!
if errorlevel 1 exit /b 1
call "!REPO!\.build-slots\env.cmd"

:after_build
if not exist "!NODELIO_BUILD_DIR!\flash_args" (
  echo [p4c5-508] missing !NODELIO_BUILD_DIR!\flash_args - full first-flash needs flash_args, not app-only
  echo [p4c5-508] run without --nofresh, or: python scripts\build.py !BOARD_DIR! --name !BUILD_NAME!
  exit /b 1
)
if not exist "!NODELIO_BUILD_DIR!\xiaozhi.bin" (
  echo [p4c5-508] missing !NODELIO_BUILD_DIR!\xiaozhi.bin
  exit /b 1
)

set "CFG=!NODELIO_SDKCONFIG!"
if not exist "!CFG!" set "CFG=sdkconfig"
if not exist "!CFG!" (
  echo [p4c5-508] missing sdkconfig
  exit /b 1
)
findstr /b /c:"CONFIG_BOARD_TYPE_SILA_P4C5_508=y" "!CFG!" >nul
if errorlevel 1 (
  echo [p4c5-508] CONFIG_BOARD_TYPE_SILA_P4C5_508 not set - refuse flash ^(wrong board build^)
  exit /b 1
)
findstr /c:"!PART_CSV!" "!CFG!" >nul
if errorlevel 1 (
  echo [p4c5-508] WARN partition table may not match !VARIANT! ^(!PART_CSV!^)
  echo [p4c5-508] 16m/32m layouts are not app-only OTA compatible - rebuild for this variant
)

if /i "!VARIANT!"=="16m" (
  findstr /b /c:"CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y" "!CFG!" >nul
  if errorlevel 1 (
    echo [p4c5-508] WARN expected CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y for --16m
  )
)

call :resolve_port
if errorlevel 1 exit /b 1
call :free_port
echo [p4c5-508] flash+monitor on !PORT! ^(P4 USB Serial/JTAG; variant=!VARIANT!; C5 NOT flashed^) ...
call "!REPO!\scripts\nodelio_idf.cmd" -p !PORT! flash monitor
exit /b %ERRORLEVEL%

:quick_build
set "CFG=!NODELIO_SDKCONFIG!"
if not exist "!CFG!" set "CFG=sdkconfig"
if not exist "!CFG!" (
  echo [p4c5-508] no sdkconfig - run once without --quick
  exit /b 1
)
findstr /b /c:"CONFIG_BOARD_TYPE_SILA_P4C5_508=y" "!CFG!" >nul
if errorlevel 1 (
  echo [p4c5-508] sdkconfig is not this board - run once without --quick
  exit /b 1
)
findstr /c:"!PART_CSV!" "!CFG!" >nul
if errorlevel 1 (
  echo [p4c5-508] sdkconfig partition is not !VARIANT! - run without --quick
  exit /b 1
)
echo [p4c5-508] incremental build + app-flash ^(!VARIANT!^) ...
call "!REPO!\scripts\nodelio_idf.cmd" build
if errorlevel 1 exit /b 1
call :resolve_port
if errorlevel 1 exit /b 1
call :free_port
echo [p4c5-508] app-flash+monitor on !PORT! ...
call "!REPO!\scripts\nodelio_idf.cmd" -p !PORT! app-flash monitor
exit /b %ERRORLEVEL%

:import_root_build_to_slot
if not exist "build\xiaozhi.bin" exit /b 0
if "!NODELIO_BUILD_DIR!"=="" exit /b 0
echo [p4c5-508] copy root build\ -^> slot !NODELIO_BUILD_DIR!
robocopy "build" "!NODELIO_BUILD_DIR!" /E /COPY:DAT /R:1 /W:1 /NFL /NDL /NJH /NJS >nul
if exist "sdkconfig" copy /Y "sdkconfig" "!NODELIO_SDKCONFIG!" >nul
exit /b 0

:resolve_port
if not "!PORT!"=="" exit /b 0
echo [p4c5-508] scanning for P4 USB Serial/JTAG ^(VID_303A^) ...
set "PORT_FILE=%TEMP%\nodelio-p4-port.txt"
del /q "!PORT_FILE!" >nul 2>&1
powershell -NoProfile -Command ^
  "$devs = @(Get-PnpDevice -Class Ports -ErrorAction SilentlyContinue | Sort-Object FriendlyName);" ^
  "if ($devs.Count -eq 0) { Write-Host '[p4c5-508] no Ports-class devices'; exit 3 }" ^
  "$jtagOk = New-Object System.Collections.Generic.List[string];" ^
  "foreach ($d in $devs) {" ^
  "  if ($d.FriendlyName -notmatch '(COM\d+)') { continue }" ^
  "  $com = $Matches[1];" ^
  "  $line = '  ' + $com + '  [' + $d.Status + ']  ' + $d.FriendlyName;" ^
  "  if ([string]$d.InstanceId -notmatch 'VID_303A') { Write-Host ($line + '  [skip]'); continue }" ^
  "  if ($d.Status -eq 'OK') { Write-Host ($line + '  [P4 USB-JTAG]'); $jtagOk.Add($com) } else { Write-Host ($line + '  [P4 USB-JTAG stale]') }" ^
  "}" ^
  "$jtagOk = @($jtagOk | Select-Object -Unique);" ^
  "if ($jtagOk.Count -eq 1) { Set-Content -Path $env:TEMP\nodelio-p4-port.txt -Value $jtagOk[0] -NoNewline; exit 0 }" ^
  "if ($jtagOk.Count -gt 1) { Write-Host '[p4c5-508] multiple P4 ports; pass COMx explicitly'; exit 2 }" ^
  "Write-Host '[p4c5-508] no live P4 USB-JTAG port. Plug in Type-C or pass COMx.'; exit 3"
if errorlevel 1 (
  echo [p4c5-508] auto port pick failed
  exit /b 1
)
if not exist "!PORT_FILE!" (
  echo [p4c5-508] auto port pick failed
  exit /b 1
)
set /p PORT=<"!PORT_FILE!"
del /q "!PORT_FILE!" >nul 2>&1
if "!PORT!"=="" (
  echo [p4c5-508] auto port pick failed
  exit /b 1
)
echo [p4c5-508] auto port !PORT!
exit /b 0

:free_port
rem Previous "flash monitor" keeps COM open after the window looks idle.
powershell -NoProfile -Command ^
  "$port = $env:PORT;" ^
  "if (-not $port) { exit 0 }" ^
  "$hits = @(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -and $_.CommandLine -match [regex]::Escape($port) -and $_.CommandLine -match 'idf_monitor|esp_idf_monitor|idf.py' });" ^
  "foreach ($p in $hits) { Write-Host ('[p4c5-508] stop ' + $p.ProcessId + ' ' + $p.Name + ' holding ' + $port); Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }"
exit /b 0
