@echo off
setlocal
cd /d "%~dp0"
rem WDK/build-project cleanup. Removes old build artifacts (obj trees,
rem build logs/err/wrn, __pycache__, .bstate).
rem Keeps exes, sources, docs, and backups. Uses recyc.exe (Recycle Bin)
rem when found, else del/rd.

set "RM="
where recyc.exe >nul 2>&1 && set "RM=recyc.exe"

call :rm objfre_win7_amd64
call :rm __pycache__
call :rm buildfre_win7_amd64.log
call :rm buildfre_win7_amd64.err
call :rm buildfre_win7_amd64.wrn
call :rm .bstate
rem smoke traces (smoketest.ps1 leaves ./tmp + transcript for inspection;
rem clean is the ONLY place that removes them). Legacy bat-era traces last.
call :rm tmp
call :rm smoke-transcript.log
call :rm smoke-report.log
exit /b 0

:rm
if not exist "%~1" exit /b 0
if defined RM (
    %RM% /q "%~1"
) else if exist "%~1\" (
    rd /s /q "%~1"
) else (
    del /f /q /a "%~1"
)
exit /b 0
