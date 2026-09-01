@echo off
REM Runs the PC build with everything turned on, for finding the thing that
REM kills the process when a 3D scene loads.
REM
REM Two switches do the work. --gl-errors asks the driver after every single
REM GL call whether it objected, and names the call that did. --log-calls
REM names every import the engine makes, from the second given onwards, with
REM the output unbuffered - so whatever the last line says is literally the
REM last thing the process did before it disappeared.
REM
REM Both are slow, and the log is enormous. That is the trade: this build is
REM for one run that answers the question, not for playing.

setlocal

if not defined WB_BUILD set "WB_BUILD=%~dp0build"
set "EXE=%WB_BUILD%\warband_nx.exe"
if not defined WB_SO set "WB_SO=%~dp0game\libMBExpMobile.so"
set "SO=%WB_SO%"
if not defined WB_DATA set "WB_DATA=%~dp0game/gamedata"
set "DATA=%WB_DATA%"
if not defined WB_SAVE set "WB_SAVE=%~dp0game/saves"
set "SAVE=%WB_SAVE%"
set "LOG=%WB_BUILD%\trace.log"

if not exist "%EXE%" (
  echo Not built yet - run build.bat
  pause
  exit /b 1
)

echo Logging to "%LOG%"
echo.
echo Import logging starts 30 seconds in, so most of the loading is in the
echo log too. Expect it to be large and loading to be slower than usual.
echo Wait for the menu, then choose Play Tutorial.
echo.

"%EXE%" "%SO%" "--data=%DATA%" "--save=%SAVE%" --svcs=0 --seconds=86400 ^
  --gl-errors --log-calls=30 %* > "%LOG%" 2>&1

echo.
echo ---- the last 25 lines ----
powershell -NoProfile -Command "Get-Content -LiteralPath '%LOG%' -Tail 25"
echo ---------------------------
echo.
echo The last [call] line names the last import the engine made.
pause

endlocal
