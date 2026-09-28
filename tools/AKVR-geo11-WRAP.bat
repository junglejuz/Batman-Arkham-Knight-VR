@echo off
rem Put geo-11 back exactly as it ships (it becomes d3d11.dll again).
rem This is the known-good stereo setup. Use it to undo the HOOK experiment.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0AKVR-geo11-mode.ps1" -Mode wrap
pause
