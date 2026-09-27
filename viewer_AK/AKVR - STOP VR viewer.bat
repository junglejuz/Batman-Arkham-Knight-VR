@echo off
REM ---------------------------------------------------------------------------
REM  Close the standalone VR viewer (batvision.exe).
REM  It does NOT exit when the game does, and while it is alive it OWNS the
REM  headset — Virtual Desktop then tells Arkham "no HMD", so AKVR's own VR
REM  mode can never start. Cost us a test cycle on 2026-08-06.
REM  The old tf2vr_viewer.exe name is killed too, in case a stale one is up.
REM ---------------------------------------------------------------------------
set FOUND=0
taskkill /F /IM batvision.exe >nul 2>&1
if not errorlevel 1 set FOUND=1
taskkill /F /IM tf2vr_viewer.exe >nul 2>&1
if not errorlevel 1 set FOUND=1
if "%FOUND%"=="1" (
    echo VR viewer closed - the headset is free again.
) else (
    echo VR viewer was not running.
)
timeout /t 2 >nul
