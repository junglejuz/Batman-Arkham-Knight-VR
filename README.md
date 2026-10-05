# Batman: Arkham Knight VR

Play Batman: Arkham Knight in PC VR with native stereo, your head moving the camera, and the
game's third-person view kept intact.

## Native stereo

Every frame, the game renders both eyes, each from its own position, at the same moment.
Nothing is reconstructed from a flat image, and the eyes never take turns, so swinging the
camera never leaves one eye behind the other. Everything in the scene, near or far, has
correct depth.

![Arkham Knight in VR: left and right eye](images/city-streets.jpg)

![Arkham Knight in VR: left and right eye](images/batmobile.jpg)

*The two eye pictures side by side, saved by the mod while playing.*

## How it works

- **geo-11** (a free stereo 3D tool, with a community fix for Arkham Knight) makes the game
  draw a separate picture for each eye.
- **This mod** shows those two pictures in your headset, moves the game's camera with your
  head, and adds an in-headset settings panel.

## What works (checked in the headset)

- Head tracking that follows the game's own up-and-down camera tilt; turning your head never
  tilts the horizon
- Smooth camera when spinning with the stick, and when getting in and out of the Batmobile
- World scale you can change while playing
- A steady HUD that hangs in the room, with depth for each eye. Single parts (radar, compass,
  radio) can be moved, resized or hidden, and your layout is remembered.
- The grapple reticle and the target distance stay on what they point at
- Rain stays in the world when you turn your head
- Menus, map, loading screens and start-up logos at comfortable sizes; when paused, the world
  stays around you, a little darker
- Zooming (right stick click) darkens the edges of your view instead of showing the game's flat
  zoom overlay

## Installing

The installer downloads the 3D fix, puts everything into the game folder (the one with
`BatmanAK.exe`) and sets it up for VR, so you don't copy or unpack anything there yourself.

### What you need

- Batman: Arkham Knight on PC
- A PC VR headset and the software you use for PC VR games, for example Virtual Desktop,
  SteamVR or the Meta Quest Link app. Tested with a Meta Quest 3 through Virtual Desktop;
  other headsets and software haven't been tested yet.
- A strong graphics card, since every picture is drawn twice. Tested on an NVIDIA RTX 4070 Ti.

### Steps

1. **Download the mod.** Under **Releases** on this page, open the newest release and download
   the `AKVR-ArkhamKnight` zip. Right-click it, choose **Extract All...**, and extract it in
   your Downloads folder.
2. **Run the installer.** Close the game, open the `AKVR-ArkhamKnight` folder and double-click
   `Install-AKVR.bat`.
   - If Windows says "Windows protected your PC", click **More info**, then **Run anyway**.
   - It downloads the 3D fix
     ([Batman_Arkham_Knight_geo11_fix.7z](https://masterotaku.s3.amazonaws.com/Batman+Arkham+Knight/Batman_Arkham_Knight_geo11_fix.7z))
     by itself. If that file is already in Downloads (still packed), it uses that copy.
   - If it can't find the game, it asks you to pick the game's folder. For Steam that's usually
     `C:\Program Files (x86)\Steam\steamapps\common\Batman Arkham Knight`.
   - It asks how sharp the picture should be: **Low**, **Medium** or **High**. Sharper needs a
     faster graphics card; High is what the mod was tested with. Not sure? Press Enter for
     Medium. You can change it later in the settings panel.
   - Wait for **Done.**, then press any key to close the window.
3. **Check your headset software (once).** It has to be the PC's active OpenXR runtime, which
   it usually already is. In Virtual Desktop, the Streamer app's **OpenXR Runtime** should be
   **VDXR**. In SteamVR or the Meta Quest Link app, look for the OpenXR setting in their
   settings.
4. **Play in VR.** Connect your headset, then start the game with the
   **Batman Arkham Knight (VR)** shortcut on your desktop and in the Start menu (with Virtual
   Desktop, from its view of your PC). The first VR start takes a few minutes while geo-11
   prepares its shaders, and the game may look frozen; let it finish. Press **F12** to face the
   view forward and **F8** for the settings panel.

Starting the game from Steam as usual plays it normally on your monitor, with your own graphics
settings; the mod doesn't run then. To always start in VR from Steam, add `-akvr` to the game's
launch options (right-click the game in Steam, **Properties**, **Launch options**).

The installer backs up everything it replaces into a `vrmod_backup_...` folder in the game
folder. Run it again at any time to repair an install or to install a new version.

### If something goes wrong

| What you see | What to do |
|---|---|
| The installer stops with a red **STOPPED** message | The message says what's missing. Most often it's the wrong fix file (the 3D Vision one instead of geo-11). |
| "could not copy the fix into the game folder" | Right-click `Install-AKVR.bat` and choose **Run as administrator**. |
| "some HUD edits could not be applied" | The 3D fix has changed since this version of the mod. Check for a newer version of the mod. |
| Stutters a lot on the first run after a restart | The game is on a slow drive. Move it to an SSD (Steam: right-click the game, Properties, Installed Files, Move install folder), then run the installer again. |
| The game never goes into the headset | Use the **Batman Arkham Knight (VR)** shortcut, connect the headset before starting the game, and check step 3. If you played another VR game first, restart the PC. |
| The game is flat (no 3D) after updating geo-11 or the fix by hand | Run the installer again. |

### Removing it

Close the game, then double-click `Uninstall-AKVR.bat` in the game folder. It removes the mod,
the 3D fix, the VR shortcuts and your VR settings, and puts back any files the install replaced.
The `vrmod_backup_...` folders stay; delete them once you're sure you don't need them.

## Graphics settings

The game draws everything twice, once for each eye, so it needs about twice the graphics power
it normally would. VR and normal play keep separate graphics settings; your normal ones are
never changed.

**Set once by the installer** (you can change them in the game's graphics menu while in VR):

- Max FPS 90. The game's default of 60 makes head movement look blurred.
- Texture resolution, shadow quality and level of detail High; texture filtering 2x anisotropic
- The four NVIDIA GameWorks effects off (Enhanced Rain, Enhanced Light Shafts, Interactive
  Smoke / Fog, Interactive Paper Debris). They cost a lot when the game is drawn twice.

**Set by the mod every time the game starts in VR:**

- Windowed mode and the picture size. Don't change the display mode or resolution in the
  game's menu.
- Motion blur, chromatic aberration, film grain and depth of field (the distance blur) off
- V-sync off (the headset sets the pace)
- A game timing setting that keeps the HUD markers and the rain in step with the world

If you turn GameWorks effects back on and the world jitters when you turn your head, try a
**head-pose delay** of 3 (F8, under **Frame rate**).

The fix has its own keys: **F6** turns the blur behind some menus on or off, and **L** turns lens flares
on or off.

## The VR settings panel

Press **F8** to show or hide the panel. It opens upright in front of you, a little below eye level.
With the controller, hold both stick clicks (L3 + R3):
the left stick moves, **A** selects or changes, **B** goes back, and both stick clicks close it.
The game ignores the controller while the panel is open. Everything you change is saved.

At the top: **Recenter (F12)** faces the view forward, and **Save capture (F2)** saves pictures
for troubleshooting.

### View

| Setting | What it does |
|---|---|
| World scale | How big the world feels. 1.00 is life size; higher makes the world feel bigger around you. |
| Extra view at the sides | Draws a little past the edges of the lenses, so no black edges show when you turn quickly. |
| Picture size per eye | Sharpness against speed, as a scale of High (1.00 = 2860 pixels tall, 0.50 to 2.01). Quick picks: Low / Medium / High (the installer's 2016 / 2432 / 2860) and, for sharper headsets and faster graphics cards, Very high / Ultra / Max (3600 / 4320 / 5760). Takes effect after a restart. |
| Aim with your head while holding the left trigger | While the left trigger is held (Batmobile battle mode, gadget aim) you aim where you look. The panel shows **AIMING** while it is on. |
| Where you stand | Moves your viewpoint left or right, down or up, back or forward. **Reset position** undoes it. |

**Smooth Batmobile get in / out**, **Soften the game's sudden camera jumps** and **Ease the
game's fast camera tilts** smooth out the game's own camera pops. They are on by default; leave
them on.

### HUD

| Setting | What it does |
|---|---|
| HUD size | How big the HUD is. |
| HUD up / down | Moves the whole HUD up or down. |
| HUD distance | How far away the HUD looks, in metres. **far away** puts it at the distance of far-off scenery. |
| Attach UI to head movement | Off (normal): the HUD hangs still in the room, and **F12** brings it back in front of you. On: the HUD moves with your head. |
| Move / resize single HUD parts | With the HUD on screen, press **find the HUD parts**. Tick **hide** on a part to see which one it is, then open it to change its size and position. **Reset** undoes a part. |

The reticle and distance-marker switches are on by default; leave them on.

### Zoom vignette

| Setting | What it does |
|---|---|
| Strength | How dark the edges get while you zoom. 0 turns it off. |
| Clear centre | How much of the middle stays clear, in degrees. |
| Preview the vignette now | Shows it without zooming. |

### Menus and screens

| Setting | What it does |
|---|---|
| Float as a screen now (Pause key) | Shows the game on a flat floating screen, for example for cutscenes. |
| Floating screen size | How big that screen is. |
| Main menu size, camera and background | How big the main menu is, where Batman stands beside it, and how dark and soft-edged the panel behind its items is. |
| Map, pause menu, start-up and loading screen sizes | How big each of those looks. |
| Pause: darken the world | How much the world darkens while paused. |

### Frame rate

| Setting | What it does |
|---|---|
| Hold the game at | A slider: 30, 36, 40, 45, 48, 50, 60, 72, 80, 90, 96, 100 or 120 frames per second, or **off** (as fast as it can) at the right end. A steady rate is smoother than an uneven one. Each rate needs a headset refresh rate that is a whole multiple of it (72 needs 72 Hz, 60 needs 120 Hz, 45 needs 90 Hz); the mod switches the headset to one when the headset software allows it, otherwise the panel says which rate to set. A Quest 3 has no rate that suits 48, 50, 96 or 100. |
| Between game frames | What the headset shows between game frames: **repeat the frame**, or **Virtual Desktop SSW** (Virtual Desktop only; set its SSW to Always). |
| Head-pose delay | Leave it at 2. |

The **Rain test** drawer and **Test tools on** are for troubleshooting only. Leave test tools
off: they can crash the game.

## Keys

| Key | What it does |
|---|---|
| F8 | Show or hide the settings panel |
| F11 | Head tracking on or off |
| F12 | Recenter |
| F2 | Save a capture, for troubleshooting |
| Pause | Show the game on a floating screen, or go back |

## Building

Needs Visual Studio 2022 and CMake. The project expects a `references` folder next to
this one, containing minhook, imgui and the OpenXR SDK.

```
cmake -S akvr -B akvr/build-dinput8 -DAKVR_PROXY=dinput8
cmake --build akvr/build-dinput8 --config Release --target akvr
```

Then run `install\Install-AKVR.bat` from the repository: it finds the `dinput8.dll` you built.
