@echo off
rem Batman: Arkham Knight VR: install the mod. Close the game first.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-AKVR.ps1" %*
pause
