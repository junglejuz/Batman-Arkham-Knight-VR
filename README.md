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
- A steady HUD on its own layer that hangs still in the room, with real depth for each eye
- The grapple reticle and the target distance stay locked to what they point at
- Rain stays in the world when you turn your head
- Pause: the world stays around you, a little darker, with the pause menu on top
- Zooming (right stick click) darkens the edges of your view instead of showing the game's
  flat zoom overlay
- Start-up logos, loading screens, pause and map shown at their own comfortable sizes

## Installing

You download the mod and the 3D fix, then run the installer. It puts everything into the game
folder (the one with `BatmanAK.exe`) and sets it up for VR, so you don't copy or unpack
anything there yourself.

### What you need

- Batman: Arkham Knight on PC
- A PC VR headset and the software you use for PC VR games, for example Virtual Desktop,
  SteamVR or the Meta Quest Link app. Tested with a Meta Quest 3 through Virtual Desktop;
  other headsets and software haven't been tested yet.
- A strong graphics card, since every picture is drawn twice. Tested on an NVIDIA RTX 4070 Ti.

geo-11 comes with the mod, so you don't need to download it.

### Steps

1. **Download the mod.** Under **Releases** on this page, open the newest release and download
   the `AKVR-ArkhamKnight` zip. Right-click it, choose **Extract All...**, and extract it in
   your Downloads folder.
2. **Download the 3D fix:**
   [Batman_Arkham_Knight_geo11_fix.7z](https://masterotaku.s3.amazonaws.com/Batman+Arkham+Knight/Batman_Arkham_Knight_geo11_fix.7z).
   Leave it in Downloads without unpacking it.
3. **Run the installer.** Close the game, open the `AKVR-ArkhamKnight` folder and double-click
   `Install-AKVR.bat`.
   - If Windows says "Windows protected your PC", click **More info**, then **Run anyway**.
   - If it can't find the game by itself, it asks you to pick the game's folder. The folder
     named after the game is fine (for Steam that's usually
     `C:\Program Files (x86)\Steam\steamapps\common\Batman Arkham Knight`); the installer
     finds the right folder inside it.
   - It asks how sharp the picture should be: **Low**, **Medium** or **High**. Sharper needs a
     faster graphics card. High is what the mod was tested with (RTX 4070 Ti). Not sure? Press
     Enter for Medium. You can change it later in the settings panel ("Picture height per eye").
   - Wait for **Done.**, then press any key to close the window.
4. **Check your headset software (once).** It has to be the PC's active OpenXR runtime, which
   it usually already is. In Virtual Desktop, the Streamer app's **OpenXR Runtime** should be
   **VDXR**. In SteamVR or the Meta Quest Link app, look for the OpenXR setting in their
   settings.
5. **Play in VR.** Connect your headset as you normally do, then start the game with the
   **Batman Arkham Knight (VR)** shortcut the installer put on your desktop and in the Start
   menu (with Virtual Desktop, from its view of your PC). It starts the game through Steam,
   in VR. The first VR start takes a few minutes while geo-11 prepares its shaders, and the
   game may look frozen; let it finish. Press **F12** to face the view forward and **F8** for
   the settings panel.

**Starting the game from Steam as usual plays it normally**, on your monitor, with your own
graphics settings: nothing of the mod runs then. The game keeps separate graphics settings
for VR and for playing normally, and switches between them by itself. To always start in VR
from Steam instead, add `-akvr` to the game's launch options in Steam (right-click the game,
**Properties**, **Launch options**).

The installer backs up everything it replaces into a `vrmod_backup_...` folder in the game
folder. Run it again at any time to repair an install, or after downloading a new version of
the mod or the fix.

### If something goes wrong

| What you see | What to do |
|---|---|
| The installer stops with a red **STOPPED** message | The message says what's missing. Most often it's the wrong fix file (the 3D Vision one instead of geo-11). |
| "could not copy the fix into the game folder" | Right-click `Install-AKVR.bat` and choose **Run as administrator**. |
| "some HUD edits could not be applied" | The 3D fix has changed since this version of the mod. Check for a newer version of the mod. Your previous files are in the `vrmod_backup` folder. |
| Stutters a lot on the first run after a restart | The game is reading from a slow drive. Moving it to an SSD (Steam: right-click the game, Properties, Installed Files, Move install folder) fixes it; run the installer again afterwards so the VR shortcut finds the new folder. |
| The game never goes into the headset | Start it with the **Batman Arkham Knight (VR)** shortcut, not from Steam. Connect the headset before starting the game, and check step 4. If you played another VR game before this one, restart the PC first. |
| The game is flat (no 3D) after updating geo-11 or the fix by hand | Run the installer again. |

### Removing it

Close the game, then double-click `Uninstall-AKVR.bat` in the game folder (the installer put
it there). It removes the mod, the 3D fix and the VR shortcuts, including your VR settings,
leaves your normal graphics settings in place, and puts back any files the first install
replaced. The `vrmod_backup_...` folders are left in place; delete
them yourself once you're sure you don't need them.

## Graphics settings

The game draws everything twice, once for each eye, so it needs about twice the graphics
power it normally would. These settings make the biggest difference.

The game keeps separate graphics settings for VR and for playing normally; your normal ones
are never changed.

**The installer sets these once for VR, on the first install** (the settings the mod was tested
with; you can change them later in the game's graphics menu while playing in VR):

- Max FPS 90. The game's own default of 60 makes head movement look blurred in the headset.
- Texture resolution, shadow quality and level of detail High, texture filtering 2x anisotropic
- The four NVIDIA GameWorks effects off (Enhanced Rain, Enhanced Light Shafts, Interactive
  Smoke / Fog, Interactive Paper Debris). They cost a lot, and the game is drawn twice.

**The mod sets these every time the game starts in VR:**

- Windowed mode and the picture size. Don't change the display mode or resolution in the
  game's menu; the mod handles both.
- Motion blur, chromatic aberration and film grain off (all three are uncomfortable in VR)
- V-sync off (the headset sets the pace instead)
- The game's frames worked out and drawn in step, so the HUD markers and the rain match the
  world

**If you turn GameWorks effects back on**, the game takes longer to draw each picture, and the
**head-pose delay** may need changing (F8, under **Frame rate**). Try 3 if the world jitters
when you turn your head.

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
| Extra view at the sides | Draws a little past the edges of the lenses, so no black edges show when you turn quickly. |
| Picture height per eye | Sharpness against speed. Higher is sharper and slower. The installer's Low / Medium / High are 2016 / 2432 / 2860. Takes effect after a restart. |
| Smooth Batmobile get in / out | On (normal): getting in and out of the Batmobile plays the game's own camera move instead of popping. Turn it off only to compare. |
| Soften the game's sudden camera jumps | On (normal): when the game's camera suddenly jumps a short way in a single frame (it does when Batman gets out of the Batmobile), the jump is spread over a fifth of a second instead of popping. Real cuts still cut. |
| Ease the game's fast camera tilts | On (normal): when the game swings the camera up or down very fast by itself (getting out of the Batmobile, some takedowns), the view follows at a comfortable speed and eases to a stop instead. Never while you're using the right stick. |
| Aim with your head while holding the left trigger | On (normal): while the left trigger is held (Batmobile battle mode, gadget aim) the weapons aim where you look. The rest of the time the game keeps its own camera, so getting in and out of the Batmobile stays smooth. The panel shows **AIMING** while it is on. |
| Where you stand | Moves your viewpoint left or right, down or up, back or forward. If you feel you're standing to the left of what you're looking at, move left/right to the right. **Reset position** undoes it. |

### HUD

| Setting | What it does |
|---|---|
| HUD size | How big the HUD is. |
| HUD up / down | Moves the whole HUD up or down. |
| HUD distance | How far away the HUD looks, in metres. 0 or **far away** puts it at the distance of far-off scenery. |
| Attach UI to head movement | Off (normal): the HUD hangs still in the room, and you can look around it; **F12** hangs it in front of you again. On: the HUD moves with your head. |
| Reticle and distance marker sit on their target | On (normal): the grapple reticle and the target distance stay locked to what they point at. Turn it off only to compare. |
| ... and face you when you turn your head | On (normal): those markers keep facing you and keep their shape when they are off to the side of your view, instead of looking turned and squashed. Turn it off only to compare. |
| ... at their object's own distance | On (normal): those markers stay at the depth of the object they point at, even when something passes behind them. Turn it off only to compare. |
| Move / resize single HUD parts | Adjust the radar, compass and other parts one by one. With the HUD on screen, press **find the HUD parts**. Tick **hide** on a part to see which one it is, then open it to change its size and position. Each part also has **stays on its target** (keeps it at the depth it points at), **hang in the room** and **hide while zoomed**. **Reset** undoes a part. Your layout is remembered; the mod comes with the tested layout. |

### Zoom vignette

| Setting | What it does |
|---|---|
| Strength | How dark the edges get while you zoom. 0 turns it off. |
| Clear centre | How much of the middle stays clear, in degrees. |
| Preview the vignette now | Shows it without zooming, to try the settings. |

### Rain test

For checking that the rain hangs in the world: pause the game, tick **show only the rain**, turn
your head, then untick it to get the picture back.

| Setting | What it does |
|---|---|
| Show only the rain | Hides everything except the rain. |
| Rain stays in the world when you turn your head | The rain fix. Keep it on. |
| Test tools on | Extra recorders for **F2** captures. They can crash the game, so leave them off unless you're asked to make a capture. |

### Menus and screens

| Setting | What it does |
|---|---|
| Float as a screen now (Pause key) | Shows the game on a flat floating screen, for example for cutscenes. |
| Floating screen size | How big that screen is. |
| Main menu size | How big the main menu looks. |
| Main menu camera: left / right | Slides the main menu's camera sideways. Minus moves Batman further right, clear of the menu text; 0 is the game's own framing. |
| Main menu: dark background behind the items | How dark the panel behind the main menu items is. 100% is the game's own; lower fades it, so it floats less like a flat square in front of you. |
| Main menu: soften the dark background's edges | How gradually that panel fades out at its edges, so no hard square outline shows when you look around. |
| Start-up: hold the sound while the game loads the menu | On (normal): while the game pauses to load the main menu, its sound waits too, so the menu's sound effect plays as the menu appears. |
| Map size | How big the map looks. |
| Pause menu size | How big the pause menu looks over the world, so you don't have to turn your head to read it. |
| Pause: darken the world | How much the world around you darkens while the game is paused. |
| Start-up screens size | How big the logos and notices at the start of the game look. |
| Loading screens size | How big the loading screens look. |
| Loading screens up / down | Moves the loading screens up or down, in degrees. |

### Frame rate

| Setting | What it does |
|---|---|
| Hold the game at | Keeps the game at a steady 45, 40 or 30 frames per second, which is smoother than an uneven rate. **Off** lets the game run as fast as it can. |
| Between game frames | What the headset shows between game frames: **repeat the frame**, or **Virtual Desktop SSW**, which makes in-between frames (Virtual Desktop only; set its SSW to Always). |
| Head-pose delay | Leave it at 2; that value was measured as correct, and it keeps the world, the HUD markers and the rain steady together. |

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

Then run `install\Install-AKVR.bat` from the repository instead of the downloaded package: it
finds the `dinput8.dll` you built by itself.

