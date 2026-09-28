@echo off
rem Switch geo-11 to HOOK mode (renamed to geo11.dll, real d3d11.dll left alone).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0AKVR-geo11-mode.ps1" -Mode hook
pause
