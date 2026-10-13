# AKN HUD rebuild plan (HR)

Written 2026-10-13 at JJ's request ("write up a plan so that it can also be accessed in subsequent sessions"). JJ approved the
direction: stop fixing HUD elements one by one; make the placement rule general so elements we have not met yet are handled
without new code. Read this before any AKN HUD / marker / detective-mode / menu-transition work. Run log: `NOTES.md` (sections
from "DETCALLOUT" 2026-10-12 to "RB2 / RB3 RESULTS" 2026-10-13). Workspace rules: `VR_ESSENTIALS.md` §6, `VR_HUD_GUIDE.md`,
`VR_WORLD_MARKERS_GUIDE.md`.

## JJ's requirements (fixed)

1. Ordinary HUD (compass, health, objective panel, prompts) stays on the room HUD panel at HUD depth, as today.
2. World markers (distance marker, reticles, enemy icons) sit on their objects, at object depth, never doubled, never flickering
   to another place (VR_WORLD_MARKERS_GUIDE §5b/§5c recipes stay: per-vertex upright card facing the game camera position).
3. **Detective mode: all its HUD elements on the same HUD depth** (JJ 2026-10-13; earlier: "the lines and the box should all be
   at HUD depth"). The scan box points at its object (direction), but at HUD depth, with its line and label on the same plane.
4. Bat transitions and fades to black fill the whole view, close (like Batman City / Origins) - from a GAME signal only.
5. No masking of pieces we do not understand (JJ 17:45: "all you've done is mask ... instead of fixing it at a fundamental level").
6. JJ does not want to test broken builds: verify on the fake headset first (JJ allows fake-headset runs when he is away; check
   with peer sessions before using Steam), hand over only what passed.

## Where things stand (2026-10-13 23:38, installed)

| Area | State | Names |
|---|---|---|
| Detective box direction | exact: the game's own box point (through the GAME's matrices, not our HUD shrink) matches the game's projected object point every frame | SCANOWN, SCANPROJ |
| Detective second line + box | one line; four plus signs drawn by AKVR at the box's corners (fake headset 21:49) | SCANBOX4 (corners by breadth-first search under the box part) |
| Distance marker / orange diamond missing (21:3x) | FIXED: a shadowed variable (`tr`) skipped the per-eye marker layers since 18:45 | trFull rename; EYESKIP diag |
| Old line hiding on the HUD tree | OFF (useless, possibly behind the marker flicker) | SCANLINE2-4, `g_scanKillOn = 0` |
| Distance-marker number elsewhere / box flicking there | FIXED at the cause (fake headset): the mark is scaled by the piece's own fade, not lost; now recognised exactly | TAGFADE (HR2 step 1); MKCARRY2 / MKSTICKY now unused fallbacks |
| Full-view transitions | OFF (the darkness rule fired on menus + detective mode, broke markers) | TRANSFULL, setting `transfull2`, panel "TEST ... (not ready)" |
| Menu movie events | logged (UGFxMoviePlayer Start / Close, RVA 5E7CD0 / 5E8080), no class names (name table not found) | MENUSTATE |
| Recorders still in the build | FLIPREC, SCANREC, SCANPIECE, HUDSIG, HUDCOVER, BLACKHUNT, PARTID, PATHREC, BATEVENT/AKEVENT | remove or gate behind `testtools` in HR5 |

## Root causes found (evidence in NOTES)

- **One HUD sheet, pieces picked out per frame by a colour mark.** The whole HUD is drawn to a room sheet; marker pieces are
  recognised per draw by an invisible colour mark on their HUD-tree part (MKTAG / kMkTag) and moved per eye (MKEYE / MKGS). The
  game's own animations (pulses, fades) change the colour the mark rides on, and Scaleform batches pieces of several parts in one
  draw, so some pieces miss the mark on some frames and land on the flat sheet in a different place. Every "duplicate / flicker /
  pops out" bug of 10-11 and 10-13 is this class (MKGLOW2-5, MKSTICKY, FLIPREC).
- **Marker world points missing on some HUD frames** (~9 % of seconds, MKGLOW notes 10-11): the marker list for a HUD frame comes
  from the game's Canvas.Project calls (RA 0x86DC0D) kept for the last TWO game frames and matched to the eye pair by stamp. When
  the list is empty, every marker piece falls to the flat sheet = the whole marker drawn "off to the side". Open question (HR1
  measures it): does the game simply not re-project every frame (its Scaleform keeps the last position), or does our pairing miss?
- **Things that were NOT the cause** (ruled out by measurement - do not retry): the stale eye picture + HUD layer double path
  (PATHREC: 0 frames in 156 s); world points in cb0 rows 0-3 of marker pieces (the shaders never read them - leftover data;
  disassembly of fd60f2d7 / 9938094a / 91e2b222 / 05154232 / 54cd897e from GlobalShaderCache-PC-D3D-SM5.bin); the Wwise event
  Play_UI_Fader_Bats (AK never posts it - all 92 events by id); "a dark HUD covering the screen" as a fade signal (fires on menus
  and detective mode).

## Principles for the rebuild

1. **Identify a piece by the HUD-tree part that drew it, decided from the tree, not from per-frame colour arithmetic.** The tree
   is exact (AKVR reads it); the problem is carrying the part's identity to the draw. Use a carrier the game never animates for
   that part, verified per part, and fall back to "unknown" (not "flat") when it is missing.
2. **Polarity (RETHINK MARKERS 2, 10-11):** a misjudged piece must cost at most a slightly wrong DEPTH, never a wrong PLACE or a
   second copy. Pieces of an on-target part are never drawn on the room sheet as well.
3. **Hold the game's state like the game does.** Scaleform keeps a marker's last position until the script moves it; we keep its
   last world point the same way (per marker, until the game sends a new one or the marker's pieces stop being drawn), instead of
   requiring a projection every frame.
4. **Signals from game state only** (menus, transitions): engine calls (UGFxMoviePlayer Start / Close, the front-most movie),
   never HUD darkness, coverage, draw kinds or sounds we have not confirmed.
5. Every step ships with its own "did it run" counter in the log, and is checked on the fake headset before JJ sees it.

## Steps

### HR1 - marker points: measure, then hold per marker (replaces MKCARRY2)
- Measure (one fake run, distance marker in view - the rooftop start does NOT show it; walk / glide toward the objective or use a
  save point where it is in view): per HUD frame, Canvas.Project calls this game frame, stamps of both kept buffers vs the pair's
  stamp, and whether the marker's pieces were drawn. Answers: game skips vs our pairing misses.
- Build: a per-marker record (world point W, last update, last HUD frame its pieces were drawn), matched across frames by W
  (game units, 300 as MKLAG does). A marker keeps its W until a new projection moves it or its pieces stop being drawn for N
  frames. Ring of 8 game frames instead of 2 for the stamp match.
- Check: fake run with the marker in view, 2 min; "marker frames without a point" = 0; headset shots every 2 s show one marker.

### HR2 - part identity carried to the draw (replaces the per-frame colour guess)
- PARTID (diagnostic, built) gives each room part a unique mark value; extend it to on-target parts and log, per HUD frame, every
  draw's part (single pieces: colour rows r12 / r13; batched pieces: their per-piece colour rows), and the draws whose part is
  unknown, with the part's own colour transform at that moment. This shows exactly when / why the mark is lost (pulse add on the
  child, alpha multiply 0, Scaleform filter pastes).
- Build from that: match on the channel the game does not animate for that part (e.g. the alpha add when the pulse changes the
  rgb add), or re-apply the mark on the CHILD node that animates (it is the composed value that reaches the shader); pieces of an
  on-target part whose mark is unreadable on a frame stay with their marker (part membership from the tree + the marker's own
  draw window), never on the room sheet.
- Check: FLIPREC = 0 pieces of on-target parts on the room sheet over a 5 min fake run with markers; JJ's flicker gone.

### HR3 - detective mode: one HUD depth for the whole callout (JJ requirement 3)
- Today: box drawn by AKVR (SCANBOX3, corners from the box's live children), our line, the game's label - all at HUD depth.
  First confirm the plus signs draw (diag "box drawn by AKVR N (corners found N)").
- Alternative to compare on the fake headset, then show JJ both: keep the GAME's whole callout (box + line + label) together on
  the HUD plane at a scale where the box's direction from the head equals the object's direction (the callout group laid out at
  the game camera's field of view instead of the shrunk HUD); the game draws everything, nothing redrawn. Cost: the label sits
  where the flat game puts it (nearer the edge of the view). Pick with JJ.
- Other detective elements (hexagon arcs, threat icons, the orange target diamond) follow the same rule: detective = HUD depth.

### HR4 - transitions from game state (bats, fade to black)
- Find AK's name storage (the UE3 { data, num, max } "None"/"ByteProperty" search failed, also read-only live): Ghidra / REA on
  BatmanAK.exe - FName::ToString / GetName callers, or the UObject name field via a known object's class name; or read the movie's
  SWF file name (USwfMovie) from the UGFxMoviePlayer object.
- Then log which movie classes Start / Close around the bats and the fade (pause -> quit, main menu -> sub-menu, Continue).
- Build: while the transition movie is open (Start ... Close), draw the HUD sheet full view, close (Origins O1D: 0.8 m, 1.6x);
  the TRANSFULL drawing code exists, only its trigger changes. Check unpause, map open / close and quit separately (memory
  arkham-bat-sound-and-unpause).

### HR5 - clean-up
- Remove the per-element patches the rebuild replaces: MKSTICKY / MKGLOW scratch tracking where HR2 covers it, MKCARRY2, SCANCUT,
  SCANLINE2-4, the HUDCOVER fade rule. Gate the recorders behind `testtools`. Panel: plain words (PANELWORDS rule), tests in their
  own drawer.

## Order and launches

HR1 and HR2 measurements share one fake-headset run (marker in view + detective mode); builds follow; a second fake run checks
both; JJ's headset check after. HR3's comparison and HR4's name lookup can go in the same runs. Keep JJ's settings backed up and
restored (runner template: `tools/Run-RB3.ps1`).

## References

- NOTES.md: DETCALLOUT, SCANOWN, SCANLINE*, REC1 / RB1 / RB2 / RB3 RESULTS, RETHINK MARKERS 2, MKGLOW2-5, MKLAG, MKEYE.
- Memory: akn-hud-two-paths-stale-frame, ak-detective-callout-depth, markers-per-vertex-card-solved, akn-marker-pieces-by-tag,
  root-cause-over-patches, arkham-bat-sound-and-unpause.
- Batman City: `batman_city/src/game/arkhamcity/menu_state.cpp` (MENUSTATE, 32-bit), BATMAN_CITY_PLAN WIPE* / BLACKFILL.
- Origins: `batman_origins/BUILD_PLAN.md` O1D (full-view bats, 0.8 m, 1.6).
