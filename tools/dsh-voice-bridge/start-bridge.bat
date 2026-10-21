@echo off
setlocal
cd /d "%~dp0"

set "EP1=%USERPROFILE%\.agents-anywhere\dsh-bridge\endpoint.json"
set "EP2=%USERPROFILE%\.dsh\agents-anywhere\bridge\endpoint.json"
if exist "%EP1%" goto :ok
if exist "%EP2%" goto :ok
echo [dsh-bridge] DSH Desktop endpoint not found.
echo   Checked: %EP1%
echo            %EP2%
echo Open DSH Desktop and enable remote control, then retry.
exit /b 1

:ok
where node >nul 2>&1
if errorlevel 1 (
  echo [dsh-bridge] node not in PATH
  exit /b 1
)

REM Default: LAN listen. Override: set DSH_BRIDGE_HOST=127.0.0.1
if "%DSH_BRIDGE_HOST%"=="" set DSH_BRIDGE_HOST=0.0.0.0
if "%DSH_BRIDGE_PORT%"=="" set DSH_BRIDGE_PORT=8790

REM 代码热更新（Node --watch）：改 marketplace/、lib/ 自动重启。关：set DSH_BRIDGE_HOT=0
REM 技能边沿推送（server --watch）：set DSH_BRIDGE_WATCH=1 + NODELIO_HOST_URL / DEVICE_ID / DEVICE_KEY
set "NODE_HOT="
if /I not "%DSH_BRIDGE_HOT%"=="0" (
  set "NODE_HOT=--watch --watch-path=marketplace --watch-path=lib --watch-preserve-output"
)
set "HINT_ARG="
if "%DSH_BRIDGE_WATCH%"=="1" set "HINT_ARG=--watch"

echo [dsh-bridge] host=%DSH_BRIDGE_HOST% port=%DSH_BRIDGE_PORT%
if defined NODE_HOT (
  echo [dsh-bridge] hot-reload ON  ^(DSH_BRIDGE_HOT=0 to disable^)
) else (
  echo [dsh-bridge] hot-reload OFF
)
if defined HINT_ARG echo [dsh-bridge] skill_hint watch ON

node %NODE_HOT% marketplace\server.mjs --host %DSH_BRIDGE_HOST% --port %DSH_BRIDGE_PORT% %HINT_ARG%
exit /b %ERRORLEVEL%
