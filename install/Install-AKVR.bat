@echo off
rem AKVR: set up the Batman: Arkham Knight 3D fix for VR and install the mod. Close the game first.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-AKVR.ps1" %*
pause
