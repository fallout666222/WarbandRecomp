@echo off
REM Configures the PC build using the C++ toolchain bundled with Visual
REM Studio 2022 Community. Nothing needs to be on PATH beforehand.
REM
REM The project path contains an "&", which cmd treats as a command
REM separator unless quoted - hence the quoting discipline below. The build
REM tree is deliberately placed somewhere without one.

setlocal

REM Every path below can be overridden from the environment, so this works on
REM a machine that is not the one it was written on:
REM
REM   set WB_BUILD=C:\somewhere\build
REM   set WB_DYNARMIC=C:\src\dynarmic
REM   set WB_BOOST=C:\src\boost
REM   set VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Professional
REM
REM setup.bat puts dynarmic and Boost under external/ and the game under
REM game/, which is where these point by default.

if not defined VSROOT set "VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if not exist "%CMAKE%" (
  echo Could not find cmake at "%CMAKE%"
  echo Install the "Desktop development with C++" workload in Visual Studio.
  exit /b 1
)

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo vcvars64 failed
  exit /b 1
)

set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
REM Somewhere without an "&" in the path: cmd treats it as a separator, and
REM not every tool in the chain quotes carefully.
if not defined WB_BUILD set "WB_BUILD=%~dp0build"
set "BUILD=%WB_BUILD%"

REM dynarmic needs Boost headers only (icl, variant) - nothing to compile.
REM Boost_NO_BOOST_CMAKE because a headers-only tree has no BoostConfig.cmake.
if not defined WB_BOOST set "WB_BOOST=%~dp0external\boost"
if not defined WB_DYNARMIC set "WB_DYNARMIC=%~dp0external\dynarmic"
set "BOOST=%WB_BOOST%"

REM Forward slashes and quotes, and both are needed. CMake reads a
REM backslash in an argument as an escape and fails to parse the path;
REM cmd reads an unquoted ampersand as a command separator, and the path
REM to this folder may well contain one.
set "DYN_C=%WB_DYNARMIC:\=/%"
set "BOOST_C=%BOOST:\=/%"

"%CMAKE%" -G Ninja -B "%BUILD%" -S "%SRC%" ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DDYNARMIC_DIR="%DYN_C%" ^
  -DBOOST_ROOT="%BOOST_C%" ^
  -DBoost_INCLUDE_DIR="%BOOST_C%" ^
  -DBoost_NO_BOOST_CMAKE=ON ^
  -DBoost_NO_SYSTEM_PATHS=ON ^
  -Wno-dev
if errorlevel 1 exit /b 1

echo.
echo Configured. Build with:
echo   build.bat
endlocal
