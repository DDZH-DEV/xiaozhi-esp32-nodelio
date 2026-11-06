@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
set "PAUSE_AT_END=0"
if "%~1"=="" set "PAUSE_AT_END=1"
chcp 65001 >nul

where go >nul 2>&1
if errorlevel 1 (
  echo go not found. Install Go 1.24+ and add it to PATH.
  call :pause_if_click
  exit /b 1
)

set "ALL=windows/amd64,windows/arm64,darwin/amd64,darwin/arm64,linux/amd64,linux/arm64"
set "TARGETS="

if not "%~1"=="" (
  call :collect %*
  goto :build
)

:menu
echo.
echo NodeLio Bridge
echo   1  Windows amd64
echo   2  Windows arm64
echo   3  macOS amd64
echo   4  macOS arm64
echo   5  Linux amd64
echo   6  Linux arm64
echo   A  all
echo   Q  quit
echo.
set "INPUT="
set /p INPUT=Select platforms (example: 1,5 or 1 5): 
if "!INPUT!"=="" goto :menu
if /i "!INPUT!"=="Q" exit /b 0
call :collect !INPUT!
if "!TARGETS!"=="" (
  echo No valid platform selected.
  goto :menu
)

:build
if "!TARGETS!"=="" (
  echo No valid platform selected.
  call :pause_if_click
  exit /b 1
)
echo.
echo targets: !TARGETS!
echo output:  %cd%\dist
echo.
go run ./cmd/package -out dist -targets "!TARGETS!"
set "EC=!ERRORLEVEL!"
if not "!EC!"=="0" (
  echo build failed, exit !EC!
  call :pause_if_click
  exit /b !EC!
)
echo.
echo done. archives are in dist\
call :pause_if_click
exit /b 0

:collect
if "%~1"=="" goto :eof
set "ITEM=%~1"
set "ITEM=!ITEM:,= !"
for %%T in (!ITEM!) do call :one %%T
shift
goto :collect

:one
set "K=%~1"
if "!K!"=="" goto :eof
if /i "!K!"=="A" set "TARGETS=!ALL!" & goto :eof
if /i "!K!"=="ALL" set "TARGETS=!ALL!" & goto :eof
if "!K!"=="1" call :add windows/amd64 & goto :eof
if "!K!"=="2" call :add windows/arm64 & goto :eof
if "!K!"=="3" call :add darwin/amd64 & goto :eof
if "!K!"=="4" call :add darwin/arm64 & goto :eof
if "!K!"=="5" call :add linux/amd64 & goto :eof
if "!K!"=="6" call :add linux/arm64 & goto :eof
echo !K! | findstr /i /r "^windows/amd64$ ^windows/arm64$ ^darwin/amd64$ ^darwin/arm64$ ^linux/amd64$ ^linux/arm64$" >nul
if not errorlevel 1 call :add !K! & goto :eof
echo skip: !K!
goto :eof

:add
if "!TARGETS!"=="" goto :add_set
echo !TARGETS! | findstr /i /c:"%~1" >nul
if not errorlevel 1 goto :eof
:add_set
if "!TARGETS!"=="" (
  set "TARGETS=%~1"
) else (
  set "TARGETS=!TARGETS!,%~1"
)
goto :eof

:pause_if_click
if "%PAUSE_AT_END%"=="1" pause
goto :eof
