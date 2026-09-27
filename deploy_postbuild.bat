@echo off
copy /Y "%~1" "%~2" >nul 2>&1
if errorlevel 1 echo Deploy skipped: file locked or game running
exit /b 0
