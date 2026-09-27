@echo off
REM Active loader changed to dinput8 on 2026-09-25. Never install both proxies.
REM Close the game before deploying.
if exist "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\version.dll" (
    echo Deploy refused: version.dll is present. Resolve the loader choice first.
    exit /b 1
)
set SRC="D:\Documents\Antigravity\VR_Modding\akvr\build-dinput8\Release\dinput8.dll"
set DST="E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\dinput8.dll"

copy /Y %SRC% %DST%
if errorlevel 1 (
    echo Deploy failed: is the game running? Close it and try again.
    exit /b 1
)
echo Deployed: %SRC% -^> %DST%
