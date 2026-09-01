@echo off
REM Builds the Switch target. Run configure_switch.bat once first.
REM
REM Handed to devkitPro's own MSYS2 bash for the same reason the configure
REM script is - see the note there.

setlocal

if defined WB_DEVKITPRO set "DKP_WIN=%WB_DEVKITPRO%"
if not defined DKP_WIN if defined DEVKITPRO if exist "%DEVKITPRO%\cmake\Switch.cmake" set "DKP_WIN=%DEVKITPRO%"
if not defined DKP_WIN if exist "C:\devkitPro\cmake\Switch.cmake" set "DKP_WIN=C:\devkitPro"
if not defined DKP_WIN if exist "D:\devkitPro\cmake\Switch.cmake" set "DKP_WIN=D:\devkitPro"
if not defined DKP_WIN if exist "H:\devkitPro\cmake\Switch.cmake" set "DKP_WIN=H:\devkitPro"
if not defined DKP_WIN (
  echo Could not find devkitPro. Set WB_DEVKITPRO to where it is installed,
  echo for instance: set WB_DEVKITPRO=C:\devkitPro
  exit /b 1
)

set "BASH=%DKP_WIN%\msys2\usr\bin\bash.exe"
REM msys2's own bin on PATH: cygpath is called unquoted below, because a
REM quoted program path inside a for /f command line is one of the few things
REM cmd cannot parse.
set "PATH=%DKP_WIN%\msys2\usr\bin;%PATH%"
if not defined WB_SWITCH_BUILD set "WB_SWITCH_BUILD=H:\dwnld\wb_switch"

if not exist "%WB_SWITCH_BUILD%\CMakeCache.txt" (
  echo Not configured yet - run configure_switch.bat
  exit /b 1
)

for /f "delims=" %%i in ('cygpath -u "%WB_SWITCH_BUILD%"') do set "BUILD_U=%%i"
"%BASH%" -lc "cmake --build '%BUILD_U%' %*"
endlocal
