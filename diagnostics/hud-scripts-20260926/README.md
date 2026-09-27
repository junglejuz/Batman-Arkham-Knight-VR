# Arkham Knight HUD scripts (extracted 2026-09-26)

Static source for the radar-flip investigation (build ROOTPITCH, PLAYBOOK_REVIEW).

How they were made (repeatable):
1. Gildor's package extractor (https://www.gildor.org/downloads, `extract.zip`; the
   site needs a browser user agent + referer for curl):
   `extract.exe -extract -filter=SwfMovie -out=out_BmGame -path=<CookedPCConsole> BmGame.upk`
   (also Startup.upk, StartupPatch.upk; `-list` shows which package holds what).
2. `carve.py` finds the `CFX` (zlib) / `GFX` header inside each `.SwfMovie` object and
   writes an uncompressed `.gfx` (in `gfx/`).
3. JPEXS FFDec 26.3.0 (`java -jar ffdec.jar -export script <out> <file.gfx>`) gives the
   ActionScript 3 in `scripts/` (Adobe `fl/` and `scaleform/` boilerplate omitted).
   Frame rendering does not work: the art is imported at runtime.

Where the HUD lives: BmGame.upk. `ModularHudBm3` is the container movie (stage 1024x720);
it loads every `HudModule*` into itself (ModuleParent.LoadModule). The button prompts are
the separate AS2 movie `StoryModeHUD.HUD` in Startup.upk.

Findings:
- No module positions anything from the screen size per frame. Only
  `GFxSetAspectRatio` -> `AdjustExtension` moves module parents (`x = WidescreenAdjust *
  band`), once per aspect change.
- Every AS3 module is a tilted 3D panel: `AnimatorFactory3D` with `matrix3D`, plus
  `root.transform.perspectiveProjection` (FOV 26-56, centre mostly 512,384; Health and
  the radial gadget menu use 275,200; the bespoke radar uses 388,208).
- The prompts movie has no 3D and never jumps. Only the 3D panels jump when the HUD is
  shrunk with an offset viewport.

`ak.py` / `ripref.py`: capstone helpers for BatmanAK.exe (disassemble a VA, find
RIP-relative references), used for the Scaleform RE in earlyres.cpp ROOTSHRINK.
