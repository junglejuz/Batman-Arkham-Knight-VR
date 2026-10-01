@echo off
rem AKVR: start Batman: Arkham Knight in VR. Starting the game from Steam as usual plays it normally (2D).
rem This leaves a note the mod reads when the game starts (it is used up at once), then asks Steam to start
rem the game. The installer puts this file in the game folder and a "Batman Arkham Knight (VR)" shortcut
rem on the desktop and in the Start menu.
tasklist /FI "IMAGENAME eq BatmanAK.exe" 2>nul | find /I "BatmanAK.exe" >nul
if not errorlevel 1 (
    echo Batman: Arkham Knight is already running. Close it, then start it in VR again.
    pause
    exit /b 1
)
echo vr>"%~dp0akvr_launch_vr.flag"
start "" "steam://rungameid/208650"
