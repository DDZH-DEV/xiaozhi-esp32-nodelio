@echo off
rem Run idf.py against the active build slot (NODELIO_BUILD_DIR / NODELIO_SDKCONFIG).
rem Usage:  call scripts\nodelio_idf.cmd build
rem         call scripts\nodelio_idf.cmd -p COM5 flash monitor
if "%NODELIO_BUILD_DIR%"=="" (
  echo [nodelio-idf] ERROR: NODELIO_BUILD_DIR unset — call nodelio_activate_build_slot.bat first
  exit /b 1
)
if "%NODELIO_SDKCONFIG%"=="" (
  echo [nodelio-idf] ERROR: NODELIO_SDKCONFIG unset
  exit /b 1
)
idf.py -B "%NODELIO_BUILD_DIR%" -D SDKCONFIG="%NODELIO_SDKCONFIG%" %*
exit /b %ERRORLEVEL%
