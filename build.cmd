@echo off
REM Builds WhoseClip. Usage: build.cmd [x64 | x86 | both]     (default: both)
REM
REM /MT links the CRT statically on purpose: the exe must run on a bare Windows
REM install with no Visual C++ Redistributable. Do not change this to /MD.
setlocal
cd /d "%~dp0"

set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" call :findvs
set "VCVARSALL=%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARSALL%" goto :novs

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=both"

if /i "%TARGET%"=="x64"  goto :do64
if /i "%TARGET%"=="x86"  goto :do86
if /i "%TARGET%"=="both" goto :doboth
echo Usage: build.cmd [x64 ^| x86 ^| both]
exit /b 1

:do64
call :build x64 x64
if errorlevel 1 exit /b 1
goto :report

:do86
call :build x86 x86
if errorlevel 1 exit /b 1
goto :report

:doboth
call :build x64 x64
if errorlevel 1 exit /b 1
call :build x86 x86
if errorlevel 1 exit /b 1
goto :report

:report
echo.
if exist "build\x64\whoseclip.exe" for %%f in (build\x64\whoseclip.exe) do echo   x64   build\x64\whoseclip.exe   %%~zf bytes
if exist "build\x86\whoseclip.exe" for %%f in (build\x86\whoseclip.exe) do echo   x86   build\x86\whoseclip.exe   %%~zf bytes
exit /b 0

:build
echo === %~2 ===
if not exist "build\%~2" mkdir "build\%~2"
cmd /c ""%VCVARSALL%" %~1 >nul 2>&1 && cl /nologo /W4 /WX /O2 /MT /utf-8 /GS /guard:cf /EHsc /DUNICODE /D_UNICODE /Fo"build\%~2\\" /Fe"build\%~2\whoseclip.exe" src\win32\main.cpp src\win32\clip.cpp /link /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT user32.lib gdi32.lib shell32.lib"
exit /b %errorlevel%

:findvs
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -products * -latest -property installationPath 2^>nul`) do set "VSDIR=%%i"
exit /b 0

:novs
echo Could not locate vcvarsall.bat. Install Visual Studio 2022 with the C++ workload.
exit /b 1
