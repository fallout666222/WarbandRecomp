@echo off
REM Runs the sanitizer build, for one purpose: finding the write that kills
REM the process when a 3D scene loads.
REM
REM The ordinary build dies there with no output whatsoever - no fault report,
REM no terminate handler, not even the atexit hook. That is what Windows does
REM when it detects heap corruption itself: it calls __fastfail, which goes
REM around every handler a process can install. Nothing inside the process can
REM report it, so the bad write has to be caught as it happens instead.
REM
REM Everything is slower here, loading most of all, and the report - if one
REM comes - is the block of text beginning "ERROR: AddressSanitizer". That
REM block names the function and the line.

setlocal

if not defined WB_ASAN_BUILD set "WB_ASAN_BUILD=%~dp0build_asan"
set "EXE=%WB_ASAN_BUILD%\warband_nx.exe"
if not defined WB_SO set "WB_SO=%~dp0game\libMBExpMobile.so"
set "SO=%WB_SO%"
if not defined WB_DATA set "WB_DATA=%~dp0game/gamedata"
set "DATA=%WB_DATA%"
if not defined WB_SAVE set "WB_SAVE=%~dp0game/saves"
set "SAVE=%WB_SAVE%"
set "LOG=%WB_ASAN_BUILD%\asan.log"

if not exist "%EXE%" (
  echo Not built. Configure and build it with:
  echo   cmake -B "%WB_ASAN_BUILD%" -S . -DWB_ASAN=ON ...
  pause
  exit /b 1
)

echo Logging to "%LOG%"
echo Play until it dies - the tutorial is what does it.
echo.

"%EXE%" "%SO%" "--data=%DATA%" "--save=%SAVE%" --svcs=0 --seconds=86400 %* > "%LOG%" 2>&1

echo.
echo ---- anything the sanitizer found ----
powershell -NoProfile -Command "$m = Select-String -Path '%LOG%' -Pattern 'ERROR: AddressSanitizer' -SimpleMatch | Select-Object -First 1; if ($m) { Get-Content '%LOG%' | Select-Object -Skip ($m.LineNumber - 1) -First 40 } else { Write-Output '(nothing - the last 30 lines instead)'; Get-Content '%LOG%' -Tail 30 }"
echo --------------------------------------
pause

endlocal
