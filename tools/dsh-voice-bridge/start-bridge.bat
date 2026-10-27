@echo off
setlocal
cd /d "%~dp0"

set "EP1=%USERPROFILE%\.agents-anywhere\dsh-bridge\endpoint.json"
set "EP1B=%USERPROFILE%\.agents-anywhere\dsh-bridge-next\endpoint.json"
set "EP2=%USERPROFILE%\.dsh\agents-anywhere\bridge\endpoint.json"
if exist "%EP1%" goto :ok
if exist "%EP1B%" goto :ok
if exist "%EP2%" goto :ok
echo [dsh-bridge] DSH Desktop endpoint not found (app may be closed).
echo   Checked: %EP1%
echo            %EP1B%
echo            %EP2%
echo   Bridge will still start; the panel can ask to open DSH.
goto :node

:ok
:node
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
REM skill_hint watch: 有 host_ws+token 或 host_url+设备凭证时默认开；关: set DSH_BRIDGE_WATCH=0
REM Host WS: set NODELIO_HOST_WS=wss://host/ws/dsh-bridge
REM          set BRIDGE_TOKEN=xxx
REM Or edit via http://127.0.0.1:PORT/config after start
set "NODE_HOT="
if /I not "%DSH_BRIDGE_HOT%"=="0" (
  set "NODE_HOT=--watch --watch-path=marketplace --watch-path=lib --watch-preserve-output"
)
set "HINT_ARG="
if /I "%DSH_BRIDGE_WATCH%"=="1" set "HINT_ARG=--watch"
if /I "%DSH_BRIDGE_WATCH%"=="0" set "HINT_ARG="

if not exist "node_modules\ws\package.json" (
  echo [dsh-bridge] installing deps ws...
  call npm install --no-fund --no-audit
  if errorlevel 1 (
    echo [dsh-bridge] npm install failed
    exit /b 1
  )
)

REM Kill previous bridge: listeners on port + node marketplace\server.mjs (--watch orphans)
echo [dsh-bridge] stopping previous instance on port %DSH_BRIDGE_PORT% ...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$port=%DSH_BRIDGE_PORT%;" ^
  "Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue |" ^
  "  ForEach-Object { $opid=$_.OwningProcess; try { Stop-Process -Id $opid -Force -ErrorAction Stop; Write-Host \"[dsh-bridge] killed PID $opid (port $port)\" } catch {} };" ^
  "Get-CimInstance Win32_Process -Filter \"Name='node.exe'\" -ErrorAction SilentlyContinue |" ^
  "  Where-Object { $_.CommandLine -match 'marketplace[\\/]+server\.mjs' } |" ^
  "  ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop; Write-Host \"[dsh-bridge] killed node PID $($_.ProcessId)\" } catch {} }"
timeout /t 1 /nobreak >nul

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
