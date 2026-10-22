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
REM lan | host_ws | both (default both)
if "%DSH_BRIDGE_MODE%"=="" set DSH_BRIDGE_MODE=both

REM Code hot reload: Node --watch. Off: set DSH_BRIDGE_HOT=0
REM skill_hint watch: set DSH_BRIDGE_WATCH=1
REM Host WS: set NODELIO_HOST_WS=wss://host/ws/dsh-bridge
REM          set BRIDGE_TOKEN=xxx
REM Or edit via http://127.0.0.1:PORT/config after start
set "NODE_HOT="
if /I not "%DSH_BRIDGE_HOT%"=="0" (
  set "NODE_HOT=--watch --watch-path=marketplace --watch-path=lib --watch-preserve-output"
)
set "HINT_ARG="
if "%DSH_BRIDGE_WATCH%"=="1" set "HINT_ARG=--watch"

if not exist "node_modules\ws\package.json" (
  echo [dsh-bridge] installing deps ws...
  call npm install --no-fund --no-audit
  if errorlevel 1 (
    echo [dsh-bridge] npm install failed
    exit /b 1
  )
)

echo [dsh-bridge] env defaults mode=%DSH_BRIDGE_MODE% host=%DSH_BRIDGE_HOST% port=%DSH_BRIDGE_PORT%
echo [dsh-bridge] config UI: http://127.0.0.1:%DSH_BRIDGE_PORT%/config
echo [dsh-bridge] config file: bridge-config.json - hot-apply on save
if defined NODE_HOT (
  echo [dsh-bridge] code hot-reload ON - set DSH_BRIDGE_HOT=0 to disable
) else (
  echo [dsh-bridge] code hot-reload OFF
)
if defined HINT_ARG echo [dsh-bridge] skill_hint watch ON
if not "%NODELIO_HOST_WS%"=="" if not "%BRIDGE_TOKEN%"=="" (
  echo [dsh-bridge] host-ws ON  %NODELIO_HOST_WS%
)

node %NODE_HOT% marketplace\server.mjs --host %DSH_BRIDGE_HOST% --port %DSH_BRIDGE_PORT% %HINT_ARG%
exit /b %ERRORLEVEL%
