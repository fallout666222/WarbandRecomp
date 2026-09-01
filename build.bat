@echo off
REM Builds the PC target. Run configure.bat once first.
setlocal

if not defined VSROOT set "VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not defined WB_BUILD set "WB_BUILD=%~dp0build"
set "BUILD=%WB_BUILD%"

if not exist "%BUILD%\CMakeCache.txt" (
  echo Not configured yet - run configure.bat
  exit /b 1
)

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
"%CMAKE%" --build "%BUILD%" %*
endlocal
