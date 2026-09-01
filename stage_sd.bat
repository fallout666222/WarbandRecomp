@echo off
REM Assembles the SD card layout, ready to copy across.
REM
REM   sdcard\switch\warband\warband_nx.nro
REM   sdcard\switch\warband\libMBExpMobile.so
REM   sdcard\switch\warband\gamedata\
REM   sdcard\switch\warband\args.txt
REM
REM Copy that switch\ folder to the root of the card. The game data is nearly
REM eight hundred megabytes, so it is linked rather than copied where Windows
REM allows it and copied where it does not - either way what ends up on the
REM card is the same tree.

setlocal

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
if not defined WB_SWITCH_BUILD set "WB_SWITCH_BUILD=%HERE%\build_switch"
set "OUT=%HERE%\sdcard\switch\warband"

if not exist "%WB_SWITCH_BUILD%\warband_nx.nro" (
  echo No .nro yet - run configure_switch.bat and build_switch.bat first.
  exit /b 1
)

mkdir "%OUT%" 2>nul
copy /y "%WB_SWITCH_BUILD%\warband_nx.nro" "%OUT%\warband_nx.nro" >nul
if exist "%HERE%\game\libMBExpMobile.so" (
  copy /y "%HERE%\game\libMBExpMobile.so" "%OUT%\libMBExpMobile.so" >nul
) else (
  echo   no engine in game\ - run setup.bat
)
if not exist "%OUT%\args.txt" copy /y "%HERE%\switch\args.txt.example" "%OUT%\args.txt" >nul

if exist "%OUT%\gamedata" (
  echo   game data already staged
) else if exist "%HERE%\game\gamedata\Modules" (
  REM A directory junction costs nothing and takes no second copy of eight
  REM hundred megabytes. It needs no privileges, unlike a symbolic link.
  mklink /J "%OUT%\gamedata" "%HERE%\game\gamedata" >nul 2>&1
  if errorlevel 1 (
    echo   copying the game data, which takes a while...
    xcopy /e /i /q /y "%HERE%\game\gamedata" "%OUT%\gamedata" >nul
  ) else (
    echo   game data linked
  )
) else (
  echo   no game data in game\ - run setup.bat
)

echo.
echo Ready in "%HERE%\sdcard".
echo Copy the switch folder inside it to the root of the SD card, then launch
echo with title takeover: hold R on a game in hbmenu.
endlocal
