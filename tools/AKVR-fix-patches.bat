@echo off
rem Apply AKVR's changes to the installed geo-11 fix (HUD depth shaders, d3dxdm.ini). Game closed.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0AKVR-fix-patches.ps1" -Mode apply
pause
