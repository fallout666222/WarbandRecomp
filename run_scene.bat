@echo off
REM Drives the game into the tutorial ground without a hand on the mouse.
REM
REM The taps are on a clock rather than on the log because a synthetic tap is
REM delivered straight into the engine's input queue, and that is the only
REM input path that still works while the desktop has painted the window over
REM with a "not responding" placeholder. A real click through FindWindow does
REM not arrive at all then.
REM
REM The times suit a cold start on this machine: the menu is up around thirty
REM seconds in, Play Tutorial at 55, the dialogue's Continue at 85. The second
REM tap on each is a spare, in case loading ran long; landing on empty
REM parchment does nothing.
REM
REM Pass extra switches on the command line - --profile and --check-verts are
REM the two worth knowing about.

setlocal

if not defined WB_BUILD set "WB_BUILD=%~dp0build"
set "EXE=%WB_BUILD%\warband_nx.exe"
if not defined WB_SO set "WB_SO=%~dp0game\libMBExpMobile.so"
set "SO=%WB_SO%"
if not defined WB_DATA set "WB_DATA=%~dp0game/gamedata"
set "DATA=%WB_DATA%"
if not defined WB_SAVE set "WB_SAVE=%~dp0game/saves"
set "SAVE=%WB_SAVE%"
set "LOG=%WB_BUILD%\scene.log"

if not exist "%EXE%" (
  echo Not built yet - run build.bat
  exit /b 1
)

echo Logging to "%LOG%"

"%EXE%" "%SO%" "--data=%DATA%" "--save=%SAVE%" --svcs=0 --seconds=200 ^
  --shot=%WB_BUILD%/scene --shot-every=15 ^
  --tap=185,247@55 --tap=185,247@70 --tap=428,482@85 --tap=428,482@100 ^
  %* > "%LOG%" 2>&1

echo exit code %ERRORLEVEL%
echo.
powershell -NoProfile -Command "Get-Content -LiteralPath '%LOG%' -Tail 20"
echo.
echo Exit code 0 is a clean shutdown. Anything else, look for [fault] in the
echo log first and then in the Windows event log - a graphics-driver timeout
echo kills the process past every handler and leaves nothing behind.

endlocal
