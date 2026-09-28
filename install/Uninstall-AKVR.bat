@echo off
rem AKVR: remove the VR mod and the 3D fix from Batman: Arkham Knight. Close the game first.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-AKVR.ps1" %*
pause
rem In the game folder the uninstaller removes its own .ps1 when it has finished; then this file goes too.
if not exist "%~dp0Uninstall-AKVR.ps1" (goto) 2>nul & del "%~f0"
