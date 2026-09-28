@echo off
rem Put the geo-11 fix files back the way they were before AKVR-fix-patches. Game closed.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0AKVR-fix-patches.ps1" -Mode undo
pause
