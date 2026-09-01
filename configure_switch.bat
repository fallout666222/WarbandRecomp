@echo off
REM Configures the Nintendo Switch build with devkitA64 and libnx.
REM
REM Only the platform layer differs from the PC build - gl_switch.cpp instead
REM of gl_win32.cpp, audio_switch.cpp instead of audio_win32.cpp, and oaknut's
REM code block replaced so JIT pages come from libnx. Everything else is the
REM same source.
REM
REM The work is handed to devkitPro's own MSYS2 bash rather than run from cmd.
REM That is not a preference: devkitPro's CMake is an MSYS2 program, its
REM toolchain file refuses to run under any other CMake, and a Windows path on
REM its command line is read as a relative one - "H:/x" becomes the current
REM directory with "H:/x" glued on the end. Inside the shell those paths are
REM already POSIX and DEVKITPRO is already set, which is the environment
REM everything here was built to expect.
REM
REM Overridable from the environment, so this works on a machine that is not
REM the one it was written on:
REM
REM   set WB_DEVKITPRO=C:\devkitPro
REM   set WB_SWITCH_BUILD=C:\wb_switch
REM   set WB_DYNARMIC=C:\src\dynarmic
REM   set WB_BOOST=C:\src\boost_1_87_0

setlocal

REM Where devkitPro is, in the spelling cmd understands. The installer's own
REM DEVKITPRO is an MSYS2 path and means nothing here, so it is only believed
REM if it happens to point at something cmd can see.
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
if not exist "%BASH%" (
  echo Could not find %BASH%
  echo Install devkitPro with the switch-dev group, then switch-mesa and
  echo switch-libdrm_nouveau, and cmake and ninja into its MSYS2:
  echo   %DKP_WIN%\msys2\usr\bin\pacman.exe -S cmake ninja
  exit /b 1
)

set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
if not defined WB_SWITCH_BUILD set "WB_SWITCH_BUILD=H:\dwnld\wb_switch"
if not defined WB_DYNARMIC set "WB_DYNARMIC=H:\dwnld\dynarmic"
if not defined WB_BOOST set "WB_BOOST=H:\dwnld\boost_1_87_0"

REM Horizon hands out code memory as two aliases - one writable, one
REM executable, at different addresses - and dynarmic assumes one. oaknut's
REM generator already supports the split; this teaches dynarmic to use it.
REM Idempotent, so running configure again is free.
python "%SRC%\switch\patch_dynarmic.py" "%WB_DYNARMIC%"
if errorlevel 1 (
  echo Could not patch dynarmic for split code memory.
  exit /b 1
)

REM Windows paths in, POSIX paths out, before anything is handed to the shell.
for /f "delims=" %%i in ('cygpath -u "%SRC%"') do set "SRC_U=%%i"
for /f "delims=" %%i in ('cygpath -u "%WB_SWITCH_BUILD%"') do set "BUILD_U=%%i"
for /f "delims=" %%i in ('cygpath -u "%WB_DYNARMIC%"') do set "DYN_U=%%i"
for /f "delims=" %%i in ('cygpath -u "%WB_BOOST%"') do set "BOOST_U=%%i"

REM CMAKE_POLICY_VERSION_MINIMUM is not optional: devkitPro ships CMake 4, and
REM dynarmic vendors robin-map, whose CMakeLists still asks for 3.0 - which
REM CMake 4 refuses outright.
"%BASH%" -lc "cmake -G Ninja -B '%BUILD_U%' -S '%SRC_U%' -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release -DWB_SWITCH=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DDYNARMIC_DIR='%DYN_U%' -DBOOST_ROOT='%BOOST_U%' -DBoost_INCLUDE_DIR='%BOOST_U%' -DBoost_NO_BOOST_CMAKE=ON -DBoost_NO_SYSTEM_PATHS=ON -Wno-dev"
if errorlevel 1 exit /b 1

echo.
echo Configured in %WB_SWITCH_BUILD%. Build with:
echo   build_switch.bat
endlocal
