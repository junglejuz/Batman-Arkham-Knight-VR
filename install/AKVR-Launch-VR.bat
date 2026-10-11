@echo off
rem Batman: Arkham Knight VR - starts the game in VR through the PC's active OpenXR runtime (for example Virtual
rem Desktop). The game draws both eyes itself. Steam must be running. Starting the game from Steam plays it normally.
tasklist /FI "IMAGENAME eq BatmanAK.exe" 2>nul | find /I "BatmanAK.exe" >nul
if not errorlevel 1 (
    echo Batman: Arkham Knight is already running. Close it, then start it again.
    pause
    exit /b 1
)
cd /d "%~dp0"
set SteamAppId=208650
start "" "%~dp0BatmanAK.exe" -akvr -akn -aknstereo
