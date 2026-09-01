@echo off
REM One-time setup: everything the build needs, into this folder.
REM
REM   setup.bat [path-to-apk] [path-to-main-obb] [path-to-patch-obb]
REM
REM With no arguments it looks for an .apk and two .obb files here and in
REM game\. What it does:
REM
REM   external\dynarmic   cloned - the recompiler
REM   external\boost      downloaded - headers only, which is all dynarmic wants
REM   game\libMBExpMobile.so   taken out of the APK
REM   game\gamedata       the two OBBs unpacked
REM
REM Nothing here downloads the game. The APK and the OBBs come from a copy you
REM own, and without them the build still succeeds and the result has nothing
REM to run.
REM
REM Everything it fetches can be pointed at instead: set WB_DYNARMIC or
REM WB_BOOST to a tree you already have and that step is skipped.

setlocal enabledelayedexpansion

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"

where git >nul 2>&1 || (echo Needs git on PATH. & exit /b 1)
where python >nul 2>&1 || (echo Needs python on PATH. & exit /b 1)

REM ---------------------------------------------------------------- dynarmic
if defined WB_DYNARMIC (
  echo dynarmic: using %WB_DYNARMIC%
) else if exist "%HERE%\external\dynarmic\src" (
  echo dynarmic: already here
) else (
  echo Cloning dynarmic...
  REM The mirror, not merryhime/dynarmic - that repository is gone. Same code,
  REM same 0BSD licence. It vendors its externals, so no --recursive.
  git clone --depth 1 https://github.com/yuzu-mirror/dynarmic "%HERE%\external\dynarmic"
  if errorlevel 1 (echo Clone failed. & exit /b 1)
)

REM ------------------------------------------------------------------- boost
REM Headers only: dynarmic wants icl and variant and compiles none of it. The
REM release archive is the reliable way to get a consistent set - assembling
REM one out of the individual boostorg repositories means chasing a dependency
REM graph forty modules wide.
if defined WB_BOOST (
  echo boost: using %WB_BOOST%
) else if exist "%HERE%\external\boost\boost\version.hpp" (
  echo boost: already here
) else (
  echo Downloading Boost headers - this is a few hundred megabytes and only
  echo happens once...
  powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ErrorActionPreference='Stop';" ^
    "$url='https://archives.boost.io/release/1.87.0/source/boost_1_87_0.zip';" ^
    "$zip=Join-Path $env:TEMP 'boost_1_87_0.zip';" ^
    "if (-not (Test-Path $zip)) { Invoke-WebRequest $url -OutFile $zip };" ^
    "$out=Join-Path '%HERE%' 'external\boost';" ^
    "New-Item -ItemType Directory -Force $out | Out-Null;" ^
    "Add-Type -AssemblyName System.IO.Compression.FileSystem;" ^
    "$z=[IO.Compression.ZipFile]::OpenRead($zip);" ^
    "foreach ($e in $z.Entries) {" ^
    "  if ($e.FullName -notmatch '^boost_1_87_0/(boost/.*)$') { continue };" ^
    "  $rel=$Matches[1];" ^
    "  $dst=Join-Path $out $rel;" ^
    "  if ($e.Length -eq 0 -and $e.FullName.EndsWith('/')) { continue };" ^
    "  New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null;" ^
    "  [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dst, $true);" ^
    "}; $z.Dispose(); Write-Host 'boost headers extracted'"
  if errorlevel 1 (echo Boost download failed. & exit /b 1)
)

REM --------------------------------------------------------------- the game
if not exist "%HERE%\game" mkdir "%HERE%\game"

set "APK=%~1"
if not defined APK for %%f in ("%HERE%\*.apk" "%HERE%\game\*.apk") do set "APK=%%~ff"
set "MAIN=%~2"
if not defined MAIN for %%f in ("%HERE%\main*.obb" "%HERE%\game\main*.obb") do set "MAIN=%%~ff"
set "PATCHOBB=%~3"
if not defined PATCHOBB for %%f in ("%HERE%\patch*.obb" "%HERE%\game\patch*.obb") do set "PATCHOBB=%%~ff"

if exist "%HERE%\game\libMBExpMobile.so" (
  echo engine: already here
) else if defined APK (
  echo Taking the engine out of "%APK%"...
  powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ErrorActionPreference='Stop';" ^
    "Add-Type -AssemblyName System.IO.Compression.FileSystem;" ^
    "$z=[IO.Compression.ZipFile]::OpenRead('%APK%');" ^
    "$e=$z.Entries | Where-Object { $_.FullName -eq 'lib/armeabi-v7a/libMBExpMobile.so' };" ^
    "if (-not $e) { $z.Dispose(); throw 'no lib/armeabi-v7a/libMBExpMobile.so in that APK' };" ^
    "[IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path '%HERE%' 'game\libMBExpMobile.so'), $true);" ^
    "$z.Dispose(); Write-Host 'engine extracted'"
  if errorlevel 1 (echo Could not read the APK. & exit /b 1)
) else (
  echo engine: no .apk found - put one here, or pass its path
)

if exist "%HERE%\game\gamedata\Modules" (
  echo data: already here
) else (
  if defined MAIN (
    echo Unpacking "%MAIN%"...
    python "%HERE%\tools\jobb_extract.py" "%MAIN%" "%HERE%\game\gamedata"
  ) else (
    echo data: no main .obb found - put one here, or pass its path
  )
  if defined PATCHOBB (
    echo Unpacking "%PATCHOBB%"...
    python "%HERE%\tools\jobb_extract.py" "%PATCHOBB%" "%HERE%\game\gamedata"
  )
)

echo.
echo ---- what is here ----
if exist "%HERE%\external\dynarmic\src" (echo   dynarmic) else (echo   dynarmic MISSING)
if exist "%HERE%\external\boost\boost\version.hpp" (echo   boost) else (
  if defined WB_BOOST (echo   boost ^(WB_BOOST^)) else (echo   boost MISSING))
if exist "%HERE%\game\libMBExpMobile.so" (echo   engine) else (echo   engine MISSING)
if exist "%HERE%\game\gamedata\Modules" (echo   game data) else (echo   game data MISSING)
echo.
echo Next: configure.bat and build.bat for Windows,
echo       configure_switch.bat and build_switch.bat for the Switch.
endlocal
