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
  head, and adds an in-headset settings panel. It uses OpenXR, the standard way PC games talk
  to VR headsets.

## Working (checked in the headset)

- Native stereo through geo-11
- Head tracking. The camera follows the game's own up-and-down tilt, and turning your
  head never tilts the horizon.
- Smooth camera when spinning with the stick
- World scale you can change live
- Radar flicker fixed
- Move, resize or hide single HUD parts (radar, compass, radio); your layout is remembered
- Menus shown at a comfortable size
- The game's graphics menu no longer undoes the VR picture size
- HUD distance you can change live, and no stutter after leaving the pause menu or map
- A steady HUD on its own layer, with the grapple reticle and objective marker at the depth
  of what they point at

## Installing

Installing takes about five minutes. You download two things, then one installer does the
rest: it puts in geo-11, the 3D fix and the mod, and sets them up for VR.

### What you need

- Batman: Arkham Knight on PC
- A **Meta Quest 3 with Virtual Desktop** (the paid Quest app, with its free Streamer app on the
  PC). This is the only setup the mod has been tested with. See "Other headsets" below.
- A strong graphics card: the game draws every picture twice, once for each eye. The mod is
  tested on an NVIDIA RTX 4070 Ti.

You don't need to download geo-11 separately. The installer brings the geo-11 version the mod
is tested with (0.7.11).

### Step 1: download the mod

1. On this page, find **Releases** on the right-hand side and click the newest one.
2. Under **Assets**, click the `AKVR-ArkhamKnight` zip file to download it.
3. Open your Downloads folder, right-click the zip file, and choose **Extract All...**, then
   **Extract**. You now have a folder called `AKVR-ArkhamKnight`.

### Step 2: download the 3D fix

The 3D itself comes from a free fix made by the 3D community. Its author only allows it to be
downloaded from its own page, so you get it from there:

1. Open **[Batman: Arkham Knight on HelixMod](https://helixmod.blogspot.com/2020/12/batman-arkham-knight-dx11.html)**.
2. Download the **geo-11 fix**, the file `Batman_Arkham_Knight_geo11_fix.7z`. Don't take the
   "3D Vision" fix: that one is for old 3D monitors.
3. Leave the file in your Downloads folder, exactly as it is. Don't unpack it; the installer
   does that.

### Step 3: run the installer

1. Close the game if it's running.
2. Open the `AKVR-ArkhamKnight` folder from Step 1.
3. Double-click `Install-AKVR.bat`. A black window opens and shows each step as it goes.
4. If Windows shows **"Windows protected your PC"**, click **More info**, then **Run anyway**.
5. If the installer can't find the game, a window asks for the game's folder. Pick the folder
   that contains `BatmanAK.exe`. For a Steam copy that's usually
   `C:\Program Files (x86)\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64`.
6. If it can't find the 3D fix in your Downloads folder, a window asks for it. Pick the
   `Batman_Arkham_Knight_geo11_fix.7z` file from Step 2.
7. Wait until the window says **Done.** (green), then press any key to close it.

What the installer did:

- Backed up every file it replaced, into a `vrmod_backup_<date and time>` folder inside the
  game folder.
- Put the 3D fix into the game folder.
- Put in geo-11 0.7.11 and its settings for VR.
- Set geo-11 up so the mod loads it, which VR needs.
- Adjusted the fix's HUD for VR: the HUD distance setting, the steady HUD, and reticles and
  markers that sit at the depth of what they point at. These are small edits to 13 of the
  fix's HUD files, made on your copy only. The fix author's originals are kept in the
  `akvr_fix_backup` folder.
- Copied in the mod (`dinput8.dll`).

### Step 4: set up Virtual Desktop (once)

1. On the PC, open the **Virtual Desktop Streamer** app.
2. Check that **OpenXR Runtime** is set to **VDXR** (the normal setting).

### Step 5: play

1. Start the Virtual Desktop Streamer on the PC, put on the headset, and connect to your PC
   in Virtual Desktop.
2. In the headset, start Batman: Arkham Knight from Steam on your PC.
3. The first start takes a few minutes while geo-11 prepares its shaders. The game may look
   frozen during that time; let it finish. Later starts are much quicker.
4. Once the game is running, the picture moves into the headset in 3D and your head moves the
   camera. Press **F12** to face the view forward, and **F8** for the settings panel (see
   "The VR settings panel" below).

### Other headsets (untested)

The mod has only been tested on a Quest 3 with Virtual Desktop. It talks to the headset through
OpenXR, the standard that SteamVR and the Meta Quest Link app also support, so it *might* work
with other headsets or with Quest Link, but nobody has checked, and it may not work at all. If
you want to try: set your headset's app (SteamVR or Meta Quest Link) as the PC's OpenXR runtime
in that app's settings, then follow Step 5 with your headset connected. Please report what
happens, working or not.

### Updating

- **A new version of the mod:** download it as in Step 1 and run its installer (Step 3).
- **A new version of the 3D fix:** download it as in Step 2, replacing the old file, and run
  the installer again.

Running the installer again never hurts: it repairs whatever is missing.

### If something goes wrong

| What you see | What to do |
|---|---|
| The installer stops with a red **STOPPED** message | Read the message: it says what's missing. The most common cause is the wrong fix file (the 3D Vision one instead of geo-11). |
| "could not copy the fix into the game folder" | Right-click `Install-AKVR.bat` and choose **Run as administrator**. |
| "some HUD edits could not be applied" | The 3D fix has changed since this version of the mod. Nothing broke: your previous files are in the `vrmod_backup` folder. Check for a newer version of the mod. |
| The game runs on the monitor but never goes into the headset | Close the game. Make sure Virtual Desktop is connected before you start the game, and that Step 4 is done, then try again. If you played another VR game or mod before this one, restart the PC first. |
| The game is flat (no 3D) after updating geo-11 or the fix by hand | Run the installer again. A geo-11 update puts back a file that stops VR from working. |

### Removing it

1. Open the game folder (the one with `BatmanAK.exe`).
2. Run the fix's own `uninstall.bat` in that folder.
3. Delete `dinput8.dll`, `geo11.dll`, and `dxgi.dll.wrapmode` (if it's there).

Your original files are in the `vrmod_backup_...` folders if you ever need them.

To keep the 3D fix but undo only the mod's HUD edits: open the `AKVR-ArkhamKnight` folder,
click the address bar, type `powershell` and press Enter, then run:

```
powershell -ExecutionPolicy Bypass -File AKVR-fix-patches.ps1 -Mode undo -GameDir "C:\path\to\Batman Arkham Knight\Binaries\Win64"
```

(with your own game folder in the quotes).

### Building the mod yourself

If you build the mod from this repository (see Building below), run `install\Install-AKVR.bat`
from the repository instead of the downloaded package. It finds the `dinput8.dll` you built by
itself.

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
| HUD distance | How far away the HUD looks, in metres. 0 or **far away** puts it at the distance of far-off scenery. |
| HUD on its own layer (steady HUD) | Draws the HUD separately, attached to your head by the headset itself, so it stays steady instead of jumping with the game's frame rate. |
| Keep the reticle at the depth it points at | With the HUD layer on: reticles and markers that point at things (grapple reticle, objective marker) stay at the depth of what they point at, while the rest of the HUD stays on the steady layer. |
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
| Between game frames | What the headset shows between game frames: **repeat the frame**, or **Virtual Desktop SSW**, which makes in-between frames (Virtual Desktop only; set its SSW to Always). |
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
