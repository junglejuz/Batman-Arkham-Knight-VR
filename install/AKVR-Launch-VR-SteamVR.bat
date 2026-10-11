@echo off
rem Batman: Arkham Knight VR through SteamVR - the same as AKVR-Launch-VR.bat, but this one game launch uses SteamVR
rem instead of the PC's active OpenXR runtime. Nothing on the PC is changed. Steam must be running.
tasklist /FI "IMAGENAME eq BatmanAK.exe" 2>nul | find /I "BatmanAK.exe" >nul
if not errorlevel 1 (
    echo Batman: Arkham Knight is already running. Close it, then start it again.
    pause
    exit /b 1
)
set "SVRJSON="
for /f "tokens=2,*" %%a in ('reg query "HKCU\SOFTWARE\Khronos\OpenXR\1" /v ActiveRuntime 2^>nul ^| find /I "steamxr"') do set "SVRJSON=%%b"
if not defined SVRJSON for /f "tokens=2,*" %%a in ('reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul ^| find /I "SteamPath"') do if exist "%%b\steamapps\common\SteamVR\steamxr_win64.json" set "SVRJSON=%%b\steamapps\common\SteamVR\steamxr_win64.json"
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
