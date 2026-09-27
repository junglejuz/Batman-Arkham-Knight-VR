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

## Installing

### What you need

- Batman: Arkham Knight
- Quest 3 with Virtual Desktop. Leave Virtual Desktop's own OpenXR setting as it is.
- The mod file, `dinput8.dll`. Put it in this repo's `install` folder, next to the installer.
  If you build the mod yourself (see Building), the installer finds it on its own.

### Step 1: download the 3D fix

The 3D comes from a free fix for geo-11, made by the 3D fix community. You download it
yourself, from the game's page on HelixMod:

**[Batman: Arkham Knight on HelixMod](https://helixmod.blogspot.com/2020/12/batman-arkham-knight-dx11.html)**

On that page, download the **geo-11** version of the fix, not the "3D Vision" one. Leave it
in your Downloads folder as it is; there's no need to unpack it.

You don't need to download geo-11 itself: the installer brings the geo-11 version this mod
was tested with.

### Step 2: run the installer

1. Close the game.
2. Double-click `install\Install-AKVR.bat`.

The installer:

- finds the game (in any Steam library) and the fix in your Downloads folder, and asks if it can't find
  either
- backs up every file it's about to replace, into a `vrmod_backup_<date>` folder inside the
  game folder
- unpacks the fix into the game folder
- puts in our tested geo-11 and its VR settings
- switches geo-11 over so the mod can load it (VR doesn't work otherwise)
- copies in the mod

Run it again at any time to repair or update an install. If the fix is ever updated, just
download the new one and run the installer again.

### Step 3: play

1. Start Virtual Desktop and connect your headset.
2. Start Batman: Arkham Knight from Steam, from the Virtual Desktop view of your PC.

The first start can take a few minutes while geo-11 prepares its shaders. Later starts are
quicker.

### Removing it

In the game folder, run the fix's own `uninstall.bat`, then delete `dinput8.dll`,
`geo11.dll` and `dxgi.dll.wrapmode` (if it's there). Your original files are in the
`vrmod_backup_...` folders.

## Graphics settings

The game draws everything twice, once for each eye, so it needs about twice the graphics
power it normally would. These settings make the biggest difference.

**The mod already sets these, every time the game starts:**

- Windowed mode and the picture size. Don't change the display mode or resolution in the
  game's menu; the mod handles both.
- Motion blur, chromatic aberration and film grain off (all three are uncomfortable in VR)
- V-sync off (the headset sets the pace instead)

**Turn these off yourself** (in the game's graphics menu, under the NVIDIA GameWorks
options). They cost a lot, and the game is drawn twice:

- Enhanced Rain
- Enhanced Light Shafts
- Interactive Smoke / Fog
- Interactive Paper Debris

**Optional:**

- Anti-aliasing: the fix's author suggests turning it off, because it softens the picture.
- The fix has its own keys: **F6** turns depth of field on or off, and **L** turns lens
  flares on or off.

## The VR settings panel

Press **F8** to show or hide the panel. To use it with the controller, hold both stick
clicks (L3 + R3): the left stick moves, **A** selects or changes, **B** goes back, and both
stick clicks close it. The game ignores the controller while the panel has it. Everything
you change is saved.

At the top: **Recenter (F12)** faces the view forward, and **Save capture (F2)** saves
pictures and a recording, for troubleshooting.

### View

| Setting | What it does |
|---|---|
| World scale | How big the world feels. 1.00 is life size. Higher makes the world feel bigger around you; lower makes it feel like a model. **1.00** resets it. |
| Decouple the camera pitch | Off (normal): the view tilts up and down with the game's camera, as in the normal game. On: the horizon stays level and only your head tilts the view. |
| Right stick up/down: extra camera height | Only with the pitch decoupled: how far the right stick can raise the camera, in metres. |
| Extra view at the sides | Draws a little past the edges of the lenses, so no black edges show when you turn quickly. |
| Extra view top and bottom | The same for the top and bottom. Takes effect after a restart. **Same as sides** matches them. |
| Picture height per eye | Sharpness against speed. Higher is sharper and slower. Takes effect after a restart. |
| Use each eye's full view | Shapes each eye's picture to match its lens, saving about 20% of the work. Takes effect after a restart. Still being worked on: the compass shows double with it on. |
| Distance alignment | Only with each eye's full view: adjust until distant things look single. **Reset** goes back to the value measured from the headset. |
| Flip eye turn | Only with each eye's full view: tick it if everything looks badly doubled. |
| Where you stand | Moves your viewpoint left or right, down or up, back or forward. If you feel you're standing to the left of what you're looking at, move left/right to the right. **Reset position** undoes it. |

### HUD

| Setting | What it does |
|---|---|
| HUD size | How big the HUD is. |
| HUD up / down | Moves the whole HUD up or down. |
| HUD distance | How far away the HUD looks, in metres. 0 or **far away** puts it at the distance of far-off scenery. New; not yet tested. |
| HUD steadiness | Lets the HUD follow your head a little more smoothly. 0 turns it off. New; not yet tested. |
| Menus and map use the full height | Lets menus and the map fill the view from top to bottom. |
| Move / resize single HUD parts | Adjust the radar, compass and other parts one by one. With the HUD on screen, press **find the HUD parts**. Tick **hide** on a part to see which one it is, then open it to change its size and position. **Reset** undoes a part. Your layout is remembered. |

### Menus and screens

| Setting | What it does |
|---|---|
| Float as a screen now (Pause key) | Shows the game on a flat floating screen, for example for cutscenes. **With head tracking** keeps head tracking on while it floats. |
| Floating screen shape / size | The shape (the picture's own, 16:9 or 21:9) and size of that screen. |
| Main menu size | How big the main menu looks. |
| Pause / map / loading size and shape | How big the pause menu, map and loading screens look, and their shape (1.78 = 16:9). |
| Main menu: live 3D, follows your head | Shows the main menu as live 3D instead of a still picture. |
| Floating screens show a flat picture | Shows floating screens in 2D instead of 3D. |

### Frame rate

| Setting | What it does |
|---|---|
| Hold the game at | Keeps the game at a steady 45, 40 or 30 frames per second, which is smoother than an uneven rate. **Off** lets the game run as fast as it can. |
| Between game frames | What the headset shows between game frames: **repeat the frame**, or **Virtual Desktop SSW**, which makes in-between frames. For SSW, set Virtual Desktop's SSW to Always. |
| Head-pose delay | Leave it at 3; that value was measured as correct. |

**Advanced** and **Diagnostics** are for testing. Leave them alone unless asked.

## Keys

| Key | What it does |
|---|---|
| F8 | Show or hide the settings panel |
| F11 | Head tracking on or off |
| F12 | Recenter |
| F2 | Save pictures and a recording, for troubleshooting |
| Pause | Show the game on a floating screen, or go back |

## Building

Needs Visual Studio 2022 and CMake. The project expects a `references` folder next to
this one, containing minhook, imgui and the OpenXR SDK.

```
cmake -S akvr -B akvr/build-dinput8 -DAKVR_PROXY=dinput8
cmake --build akvr/build-dinput8 --config Release --target akvr
```

Then run the installer (see Installing): it picks up the built `dinput8.dll` by itself.

## Notes in this repo

- `CLAUDE_HANDOFF.md`: latest status and next steps
- `PLAYBOOK_REVIEW.md`: detailed log of every build
- `GEO11_PLAN.md`, `CAMERA_MAP.md`: background on the 3D setup and the camera
