@echo off
REM Build the EGtools Qt front-end with MSVC + Qt6 (Ninja generator).
REM Loads the MSVC environment first so CMake/Ninja find cl.exe + the Windows SDK.
setlocal

set "VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community"
set "QTDIR=C:\Qt\6.11.2\msvc2022_64"
set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "SRC=%~dp0"

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo [build] vcvars64 failed & exit /b 1)

rem Ensure ninja is reachable (falls back to a bundled copy if needed).
where ninja >nul 2>&1
if errorlevel 1 (
  echo [build] ninja not found on PATH
  exit /b 2
)

echo [build] configuring...
"%CMAKE%" -S "%SRC%" -B "%SRC%build" -G Ninja -DCMAKE_PREFIX_PATH="%QTDIR%" -DCMAKE_BUILD_TYPE=Release || exit /b 3

echo [build] compiling...
"%CMAKE%" --build "%SRC%build" || exit /b 4

echo [build] done.
exit /b 0