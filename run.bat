@echo off
REM Runs the PC build so you can play it, and keeps the log.
REM
REM Controls: the mouse is a finger - click to tap, drag to swipe. The
REM keyboard maps to Android keycodes, Escape being Back. A gamepad, if one is
REM plugged in, is read through XInput and arrives looking like the SHIELD
REM controller this build of the engine was written for.
REM
REM Loading to the menu takes about half a minute: every shader, mesh and
REM texture goes through the recompiler on the way in. The window may be
REM painted over with a "not responding" placeholder while that happens - it
REM is drawing behind it, and becomes responsive once the menu is up.
REM
REM Closing the window ends the run cleanly. So does the hour deadline below;
REM raise it with --seconds= on the command line if you want longer.
REM
REM The log is the point of this script. A crash is worth nothing without it,
REM and a console window that closes with the process takes it away - so
REM everything goes to a file, and the tail is shown afterwards either way.

setlocal

if not defined WB_BUILD set "WB_BUILD=H:\dwnld\wb_build"
set "EXE=%WB_BUILD%\warband_nx.exe"
if not defined WB_SO set "WB_SO=%~dp0..\apk_lib\libMBExpMobile.so"
set "SO=%WB_SO%"
if not defined WB_DATA set "WB_DATA=%~dp0../gamedata"
set "DATA=%WB_DATA%"
if not defined WB_SAVE set "WB_SAVE=%~dp0../usersave"
set "SAVE=%WB_SAVE%"
set "LOG=%WB_BUILD%\warband.log"

if not exist "%EXE%" (
  echo Not built yet - run build.bat
  pause
  exit /b 1
)
if not exist "%SO%" (
  echo Cannot find the engine at "%SO%"
  echo Copy libMBExpMobile.so out of the APK's lib\armeabi-v7a there.
  pause
  exit /b 1
)

echo Logging to %LOG%
echo.
echo Wait for the menu, then Play Tutorial and Continue. Close the window
echo when you are done.
echo.

REM --svcs=0 removes the import-call cap, which only exists for bisecting a
REM hang. --seconds is the deadline: a run that goes wrong ends by itself
REM rather than leaving a window nothing can close.
"%EXE%" "%SO%" "--data=%DATA%" "--save=%SAVE%" --svcs=0 --seconds=3600 %* > "%LOG%" 2>&1

echo.
echo ---- the last 40 lines of %LOG% ----
powershell -NoProfile -Command "Get-Content -Path '%LOG%' -Tail 40"
echo ------------------------------------
echo.
echo A crash leaves [fault] lines above; a clean shutdown leaves
echo "[run ] process exiting". The whole log is in %LOG%.
echo.
echo If the process vanishes with nothing in the log at all, the graphics
echo driver killed it: look in the Windows event log, not here.
pause

endlocal
