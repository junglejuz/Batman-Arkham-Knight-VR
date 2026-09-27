@echo off
REM ---------------------------------------------------------------------------
REM  AKVR — view geo-11's Katanga stereo in the headset, using OUR viewer.
REM  batvision.exe is the TF2VR viewer, renamed for this project (2026-08-06).
REM  Deliberately OUTSIDE the game folder: deploying next to the game
REM  reintroduced a geo-11 conflict on Titanfall (measured 2026-07-28).
REM  Start Arkham Knight FIRST, get to a picture, then run this.
REM
REM  NOTE: this viewer is the FALLBACK path. It takes the headset for itself,
REM  so AKVR's own OpenXR submission (per-eye poses, world-locked menus, 6DOF)
REM  cannot run at the same time. Use "AKVR - STOP VR viewer.bat" before
REM  testing the in-game VR mode.
REM ---------------------------------------------------------------------------
cd /d "%~dp0"
REM A stale instance survives the game closing and silently holds the headset.
taskkill /F /IM batvision.exe >nul 2>&1
taskkill /F /IM tf2vr_viewer.exe >nul 2>&1
start "" "%~dp0batvision.exe" --src-aspect=0 --distance=2.0 --width=3.0
