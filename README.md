# Batman: Arkham Knight VR

Play Batman: Arkham Knight in VR on a Quest 3, in true 3D, with your head moving the
camera, while keeping the game's third-person view.

## True 3D, drawn by the game itself

This is the heart of the mod. The game draws a complete, separate picture for each eye,
from each eye's own position, at the same moment. Nothing is faked from a flat image, and
the eyes never take turns. So everything on screen, near or far, has real depth, and
turning the camera never leaves one eye behind the other.

![Arkham Knight in VR: left and right eye](images/city-streets.jpg)

![Arkham Knight in VR: left and right eye](images/batmobile.jpg)

*The two eye pictures side by side, saved by the mod while playing.*

## How it works

- **geo-11** (a free stereo 3D tool, with a community fix for Arkham Knight) makes the game
  draw a separate picture for each eye.
- **This mod** shows those two pictures in the headset through Virtual Desktop, moves the
  game's camera with your head, and adds an in-headset settings panel.

## Working (checked in the headset)

- True 3D through geo-11
- Head tracking. The camera follows the game's own up-and-down tilt, and turning your
  head never tilts the horizon.
- Smooth camera when spinning with the stick
- World scale you can change live
- Radar flicker fixed
- Move, resize or hide single HUD parts (radar, compass, radio); your layout is remembered
- Menus shown at a comfortable size
- The game's graphics menu no longer undoes the VR picture size

## Still being worked on

- Built, not yet tested: HUD distance slider, stutter fix after leaving the pause menu or map
- Stutter on the main menu when the game first starts
- The compass shows double when each eye gets its own wider view
- Frame generation (extra in-between frames) still stutters (on hold for now)

## What you need

- Batman: Arkham Knight (Steam)
- Quest 3 with Virtual Desktop
- geo-11 with the Arkham Knight fix, installed in hook mode

## Keys

| Key | What it does |
|---|---|
| F8 | Show or hide the settings panel |
| F11 | Head tracking on or off |
| F12 | Recenter |

## Building

Needs Visual Studio 2022 and CMake. The project expects a `references` folder next to
this one, containing minhook, imgui and the OpenXR SDK.

```
cmake -S akvr -B akvr/build-dinput8 -DAKVR_PROXY=dinput8
cmake --build akvr/build-dinput8 --config Release --target akvr
```

With the game closed, copy `build-dinput8/Release/dinput8.dll` next to `BatmanAK.exe`.
To uninstall, delete that file.

## Notes in this repo

- `CLAUDE_HANDOFF.md`: latest status and next steps
- `PLAYBOOK_REVIEW.md`: detailed log of every build
- `GEO11_PLAN.md`, `CAMERA_MAP.md`: background on the 3D setup and the camera
