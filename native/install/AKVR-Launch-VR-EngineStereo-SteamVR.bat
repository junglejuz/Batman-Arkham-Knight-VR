@echo off
rem AKVR ENGINE STEREO through STEAMVR (2026-10-09): the same as AKVR-Launch-VR-EngineStereo.bat, but this one game
rem launch uses SteamVR instead of the PC's normal VR runtime (Virtual Desktop). Nothing on the PC is changed.
rem Connect the headset first (in Virtual Desktop: just connected, no game started). Steam must be running.
tasklist /FI "IMAGENAME eq BatmanAK.exe" 2>nul | find /I "BatmanAK.exe" >nul
if not errorlevel 1 (
    echo Batman: Arkham Knight is already running. Close it, then start it again.
    pause
    exit /b 1
)
set "SVRJSON="
for /f "tokens=2,*" %%a in ('reg query "HKCU\SOFTWARE\Khronos\OpenXR\1" /v ActiveRuntime 2^>nul ^| find /I "steamxr"') do set "SVRJSON=%%b"
if not defined SVRJSON if exist "%ProgramFiles(x86)%\Steam\steamapps\common\SteamVR\steamxr_win64.json" set "SVRJSON=%ProgramFiles(x86)%\Steam\steamapps\common\SteamVR\steamxr_win64.json"
if not defined SVRJSON (
    echo SteamVR was not found. Install SteamVR from Steam, start it once, then try again.
    pause
    exit /b 1
)
cd /d "%~dp0"
set "XR_RUNTIME_JSON=%SVRJSON%"
set SteamAppId=208650
start "" "%~dp0BatmanAK.exe" -akvr -akn -aknstereo
