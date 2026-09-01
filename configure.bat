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
REM   set WB_BUILD=C:\wb_build
REM   set WB_DYNARMIC=C:/src/dynarmic
REM   set WB_BOOST=C:/src/boost_1_87_0
REM   set VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Professional

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
if not defined WB_BUILD set "WB_BUILD=H:\dwnld\wb_build"
set "BUILD=%WB_BUILD%"

REM dynarmic needs Boost headers only (icl, variant) - nothing to compile.
REM Boost_NO_BOOST_CMAKE because a headers-only tree has no BoostConfig.cmake.
if not defined WB_BOOST set "WB_BOOST=H:/dwnld/boost_1_87_0"
if not defined WB_DYNARMIC set "WB_DYNARMIC=H:/dwnld/dynarmic"
set "BOOST=%WB_BOOST%"

"%CMAKE%" -G Ninja -B "%BUILD%" -S "%SRC%" ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DDYNARMIC_DIR=%WB_DYNARMIC% ^
  -DBOOST_ROOT=%BOOST% ^
  -DBoost_INCLUDE_DIR=%BOOST% ^
  -DBoost_NO_BOOST_CMAKE=ON ^
  -DBoost_NO_SYSTEM_PATHS=ON ^
  -Wno-dev
if errorlevel 1 exit /b 1

echo.
echo Configured. Build with:
echo   build.bat
endlocal
