@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul

rem SILA P4C5 6.2 - flash ESP32-C5 Hosted slave only (NOT the P4 host)
rem
rem Physical U14 header (callout 3), board upright, C5 module lower-right:
rem   LEFT -> RIGHT:  BOOT | EN | RXD0 | TXD0 | GND | 3V3(square pad)
rem   Silk "BOOT" is under the LEFT end. Square pad on the RIGHT = 3V3 = schematic pin1.
rem
rem   nodelio-flash-c5.bat COM13 --nofresh --no-reset
rem
rem Do NOT use board Boot key (callout 10 = P4). Do NOT flash C5 via Type-C (callout 8).

set "PORT="
set "NOFRESH=0"
set "DOMON=0"
set "ALLOW_JTAG=0"
set "RETARGET=0"
set "NORESET=0"
rem NOTE: do not put SHIFT inside if (...); it does not affect %%1 there
:parse
if "%~1"=="" goto :parsed
rem tolerate pasted flags without spaces, e.g. --nofresh--no-reset
set "ARG=%~1"
if /I "!ARG!"=="--nofresh" set "NOFRESH=1"& shift& goto :parse
if /I "!ARG!"=="--monitor" set "DOMON=1"& shift& goto :parse
if /I "!ARG!"=="--allow-jtag" set "ALLOW_JTAG=1"& shift& goto :parse
if /I "!ARG!"=="--retarget" set "RETARGET=1"& shift& goto :parse
if /I "!ARG!"=="--no-reset" set "NORESET=1"& shift& goto :parse
echo !ARG!| findstr /I /C:"nofresh" >nul && set "NOFRESH=1"
echo !ARG!| findstr /I /C:"no-reset" >nul && set "NORESET=1"
echo !ARG!| findstr /I /C:"monitor" >nul && set "DOMON=1"
echo !ARG!| findstr /I /C:"allow-jtag" >nul && set "ALLOW_JTAG=1"
echo !ARG!| findstr /I /C:"retarget" >nul && set "RETARGET=1"
echo !ARG!| findstr /I /C:"--" >nul
if not errorlevel 1 (
  shift
  goto :parse
)
set "PORT=%~1"
shift
goto :parse
:parsed

if not "!PORT!"=="" goto have_port

echo [c5-slave] scanning serial ports - need U14 USB-UART; P4 Type-C VID_303A skipped ...
set "PORT_FILE=%TEMP%\nodelio-c5-port.txt"
del /q "!PORT_FILE!" >nul 2>&1
set "ALLOW_JTAG=!ALLOW_JTAG!"
powershell -NoProfile -Command ^
  "$allowJtag = $env:ALLOW_JTAG;" ^
  "$devs = @(Get-PnpDevice -Class Ports -ErrorAction SilentlyContinue | Sort-Object FriendlyName);" ^
  "if ($devs.Count -eq 0) { Write-Host '[c5-slave] no Ports-class devices'; exit 3 }" ^
  "$uart = New-Object System.Collections.Generic.List[string];" ^
  "$uartOk = New-Object System.Collections.Generic.List[string];" ^
  "$jtag = New-Object System.Collections.Generic.List[string];" ^
  "foreach ($d in $devs) {" ^
  "  if ($d.FriendlyName -notmatch '(COM\d+)') { continue }" ^
  "  $com = $Matches[1];" ^
  "  $line = '  ' + $com + '  [' + $d.Status + ']  ' + $d.FriendlyName;" ^
  "  if ([string]$d.InstanceId -match 'VID_303A') { Write-Host ($line + '  [P4 USB-JTAG skip]'); $jtag.Add($com); continue }" ^
  "  if ([string]$d.FriendlyName -match 'SPRD') { Write-Host ($line + '  [phone modem skip]'); continue }" ^
  "  Write-Host ($line + '  [USB-UART candidate]'); $uart.Add($com);" ^
  "  if ($d.Status -eq 'OK') { $uartOk.Add($com) }" ^
  "}" ^
  "$uart = @($uart | Select-Object -Unique);" ^
  "$uartOk = @($uartOk | Select-Object -Unique);" ^
  "$jtag = @($jtag | Select-Object -Unique);" ^
  "if ($uart.Count -eq 1) { Set-Content -Path $env:TEMP\nodelio-c5-port.txt -Value $uart[0] -NoNewline; Write-Host ('[c5-slave] pick USB-UART ' + $uart[0]); exit 0 }" ^
  "if ($uartOk.Count -eq 1) { Set-Content -Path $env:TEMP\nodelio-c5-port.txt -Value $uartOk[0] -NoNewline; Write-Host ('[c5-slave] pick OK USB-UART ' + $uartOk[0] + ' (only Status=OK candidate)'); exit 0 }" ^
  "if ($uart.Count -gt 1) { Write-Host '[c5-slave] multiple USB-UART candidates; pass COMx explicitly'; exit 2 }" ^
  "if ($allowJtag -eq '1' -and $jtag.Count -eq 1) { Set-Content -Path $env:TEMP\nodelio-c5-port.txt -Value $jtag[0] -NoNewline; Write-Host ('[c5-slave] --allow-jtag using ' + $jtag[0]); exit 0 }" ^
  "Write-Host '[c5-slave] no USB-UART candidate. Pass COMx explicitly.'; exit 3"
if errorlevel 1 (
  echo [c5-slave] auto port pick failed
  exit /b 1
)
if not exist "!PORT_FILE!" (
  echo [c5-slave] auto port pick failed
  exit /b 1
)
set /p PORT=<"!PORT_FILE!"
del /q "!PORT_FILE!" >nul 2>&1
if "!PORT!"=="" (
  echo [c5-slave] auto port pick failed
  exit /b 1
)
echo [c5-slave] auto port !PORT!

:have_port

set "REPO=D:\xz25"
set "SLAVE_SRC=!REPO!\managed_components\espressif__esp_hosted\slave"
set "BUILD_DIR=!REPO!\build-c5-hosted-slave"
set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v6.1"
set "IDF_TOOLS_PATH=D:\Espressif"
set "IDF_GITHUB_ASSETS=dl.espressif.com/github_assets"
set "PATH=D:\Espressif\python_env\idf6.1_py3.11_env\Scripts;D:\Espressif\tools\idf-python\3.11.2;%PATH%"

if not exist "!SLAVE_SRC!\CMakeLists.txt" (
  echo [c5-slave] missing !SLAVE_SRC!
  echo [c5-slave] build P4 host once so component manager fetches esp_hosted
  exit /b 1
)

call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1

cd /d "!SLAVE_SRC!"

if "!NOFRESH!"=="1" goto after_build

if "!RETARGET!"=="1" goto do_retarget
if not exist "!BUILD_DIR!\build.ninja" goto do_retarget
findstr /i /c:"IDF_TARGET" "!BUILD_DIR!\CMakeCache.txt" 2>nul | findstr /i "esp32c5" >nul
if errorlevel 1 goto do_retarget
echo [c5-slave] reuse existing esp32c5 build dir - pass --retarget to fullclean
goto do_build

:do_retarget
echo [c5-slave] set-target esp32c5
idf.py -B "!BUILD_DIR!" set-target esp32c5
if errorlevel 1 exit /b 1

:do_build
echo [c5-slave] build Hosted slave into !BUILD_DIR! ...
idf.py -B "!BUILD_DIR!" build
if errorlevel 1 exit /b 1

:after_build
if not exist "!BUILD_DIR!\flash_args" (
  echo [c5-slave] missing !BUILD_DIR!\flash_args - run without --nofresh
  exit /b 1
)

echo.
echo [c5-slave] BUILD OK. Do this on header 3 BEFORE pressing a key:
echo [c5-slave]   LEFT-^>RIGHT: BOOT  EN  RXD0  TXD0  GND  3V3-square
echo [c5-slave]   1. UNPLUG board Type-C callout 8
echo [c5-slave]   2. Programmer: 3V3-^>right square, GND-^>next to square
echo [c5-slave]      RX-^>TXD0, TX-^>RXD0  [cross; swap those two if noise]
echo [c5-slave]   3. SHORT left BOOT hole to GND hole, KEEP shorted
echo [c5-slave]   4. Tap EN hole to GND once, release EN, keep BOOT shorted
echo [c5-slave]   5. Press a key now. Prefer: --nofresh --no-reset
echo [c5-slave] Press any key to probe+flash on !PORT!
pause >nul

set "BEFORE=default-reset"
if "!NORESET!"=="1" set "BEFORE=no-reset"

echo [c5-slave] probe on !PORT! before=!BEFORE! ...
python -m esptool --chip esp32c5 --port !PORT! --before !BEFORE! chip-id > "!TEMP!\nodelio-c5-chip.txt" 2>&1
set "PROBE_EC=!ERRORLEVEL!"
type "!TEMP!\nodelio-c5-chip.txt"

findstr /i /c:"Wrong boot mode" "!TEMP!\nodelio-c5-chip.txt" >nul
if not errorlevel 1 goto refuse_bootmode
findstr /i /c:"Invalid head of packet" "!TEMP!\nodelio-c5-chip.txt" >nul
if not errorlevel 1 goto refuse_noise
findstr /i /c:"No serial data" "!TEMP!\nodelio-c5-chip.txt" >nul
if not errorlevel 1 goto refuse_nodata
findstr /i /c:"could not open port" "!TEMP!\nodelio-c5-chip.txt" >nul
if not errorlevel 1 goto refuse_nodata
findstr /i /c:"Failed to connect" "!TEMP!\nodelio-c5-chip.txt" >nul
if not errorlevel 1 goto refuse_fail
if not "!PROBE_EC!"=="0" goto refuse_fail
findstr /i /c:"Detected chip" /c:"Chip is ESP32-C5" /c:"Chip type: ESP32-C5" "!TEMP!\nodelio-c5-chip.txt" >nul
if errorlevel 1 goto refuse_notc5
del /q "!TEMP!\nodelio-c5-chip.txt" >nul 2>&1
goto do_flash

:refuse_bootmode
echo [c5-slave] REFUSE: UART OK but not download mode.
echo [c5-slave] Unplug Type-C, short LEFT BOOT to GND, tap EN, keep BOOT, use --no-reset
exit /b 1

:refuse_noise
echo [c5-slave] REFUSE: Invalid head of packet - noise or TX/RX still wrong or not in download.
echo [c5-slave] Confirm LEFT-^>RIGHT: BOOT EN RXD0 TXD0 GND 3V3-square
echo [c5-slave] Swap RXD0/TXD0 wires once, keep BOOT shorted, retry --nofresh --no-reset
exit /b 1

:refuse_nodata
echo [c5-slave] REFUSE: no handshake - check GND and TX/RX
exit /b 1

:refuse_fail
echo [c5-slave] REFUSE: esptool connect failed on !PORT!
exit /b 1

:refuse_notc5
findstr /i /c:"ESP32-P4" "!TEMP!\nodelio-c5-chip.txt" >nul
if not errorlevel 1 (
  echo [c5-slave] REFUSE: that COM is P4 USB-JTAG
  exit /b 1
)
echo [c5-slave] REFUSE: did not report ESP32-C5
exit /b 1

:do_flash
echo [c5-slave] flash on !PORT! before=!BEFORE! ...
echo [c5-slave] keep LEFT BOOT shorted to GND until flash finishes
cd /d "!BUILD_DIR!"
if "!NORESET!"=="1" goto flash_noreset
if "!DOMON!"=="1" goto flash_mon
idf.py -B "!BUILD_DIR!" -p !PORT! flash
exit /b %ERRORLEVEL%

:flash_noreset
python -m esptool --chip esp32c5 -p !PORT! -b 115200 --before no-reset --after hard-reset write-flash "@flash_args"
exit /b %ERRORLEVEL%

:flash_mon
idf.py -B "!BUILD_DIR!" -p !PORT! flash monitor
exit /b %ERRORLEVEL%
