@echo off
setlocal
rem Project-local WDK build entry point. Generic, works for all projects, double clickable.
rem pattern: initialize the WDK environment, restore this project directory,
rem and pass all build arguments through to the CWD-aware Python harness.

if defined BASEDIR goto have_wdk
set "WDK_ROOT=C:\WinDDK7600.16385.1"

call %WDK_ROOT%\bin\setenv.bat %WDK_ROOT% fre x64 WIN7 no_oacr
if errorlevel 1 exit /b 1

rem setenv.bat changes the current directory, so restore this project folder.
cd /d "%~dp0"
if errorlevel 1 exit /b 1

rem CWD-aware harness if present, else plain WDK build.
if exist "..\b.py" goto have_harness
echo [make] ..\b.py not found - falling back to plain "build -gcZ %*"
build -gcZ %*
exit /b %ERRORLEVEL%

:have_harness
python ..\b.py %*
exit /b %ERRORLEVEL%
