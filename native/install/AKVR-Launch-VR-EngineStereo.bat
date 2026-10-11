@echo off
rem AKVR ENGINE STEREO (experimental, 2026-10-07): Batman: Arkham Knight in VR with the game drawing both eyes itself
rem (no geo-11). Steam must be running. The normal "Batman Arkham Knight (VR)" shortcut (geo-11) is unchanged.
tasklist /FI "IMAGENAME eq BatmanAK.exe" 2>nul | find /I "BatmanAK.exe" >nul
if not errorlevel 1 (
    echo Batman: Arkham Knight is already running. Close it, then start it again.
    pause
    exit /b 1
)
cd /d "%~dp0"
set SteamAppId=208650
start "" "%~dp0BatmanAK.exe" -akvr -akn -aknstereo
