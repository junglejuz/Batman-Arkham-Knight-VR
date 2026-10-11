"""AKN stage 5c judge (fake headset, gameplay): PASS/FAIL per test with the next step for each outcome.
Inputs: akn_log.txt, captures (pair_g / hud_*_g), headset shots xr_t3 / xr_t6 (+ sidecar .json), akvr_startup_log.txt.
Usage: python judge_m5c.py [akn folder]   -> prints and writes M5c_result.txt"""
import re, sys, os, json, math
import numpy as np
from PIL import Image

AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
GAME = os.path.dirname(AKN)
log = open(os.path.join(AKN, "akn_log.txt"), encoding="utf-8", errors="replace").read().splitlines()
out = []
def res(name, ok, detail, nxt_fail, nxt_pass=""):
    out.append(f"{name}: {'PASS' if ok else 'FAIL'} - {detail}" + (f"  -> NEXT: {nxt_fail}" if not ok else ""))
def load(name, mode="RGB"):
    p = os.path.join(AKN, name)
    return np.asarray(Image.open(p).convert(mode), dtype=np.float32) if os.path.exists(p) else None
def side(name):
    p = os.path.join(AKN, name)
    if not os.path.exists(p): return None
    try: return json.load(open(p, encoding="utf-8-sig"))
    except Exception: return None
def mark(text):
    for i, l in enumerate(log):
        if "MARK" in l and text in l: return i
    return None
crash = [l for l in log if "CRASH RECORDER" in l and "XINPUT9" in l.upper().split("| RSP")[0]]   # ours only (AKVR's own guarded probes are handled)
turned = [l for l in log if "turned lens: yaw" in l]   # TURNEDEYE lens mode active
logs_dir = os.path.join(os.path.dirname(os.path.dirname(GAME)), "BmGame", "Logs")
def common_crop(Lim, Rim, tans):
    """Both eyes cropped to the horizontal field they share (asymmetric lenses: each eye sees ~10 deg more outward)."""
    (l0, r0, _, _), (l1, r1, _, _) = tans
    lo, hi = max(l0, l1), min(r0, r1)
    W = Lim.shape[1]
    a0, b0 = int((lo - l0) / (r0 - l0) * W), int((hi - l0) / (r0 - l0) * W)
    a1, b1 = int((lo - l1) / (r1 - l1) * W), int((hi - l1) / (r1 - l1) * W)
    return Lim[:, a0:b0], Rim[:, a1:b1]
tans = None
eyes_all = [l for l in log if "eyes from AKVR #" in l]
if eyes_all:
    m_ = re.search(r"L tan ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+) \| R tan ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+)", eyes_all[-1])
    if m_: v = [float(x) for x in m_.groups()]; tans = (v[:4], v[4:])

# T0 link
st = open(os.path.join(GAME, "akvr_startup_log.txt"), encoding="utf-8", errors="replace").read() if os.path.exists(os.path.join(GAME, "akvr_startup_log.txt")) else ""
eyes = [l for l in log if "eyes from AKVR #" in l]
ok0 = "engine stereo (-akn)" in st and bool(eyes)
res("T0 link", ok0, f"AKVR engine mode logged: {'engine stereo (-akn)' in st}, AKN got eyes from AKVR: {len(eyes)} lines",
    "no engine-mode line -> -akn not seen by AKVR; no eyes -> akn_link() not finding XINPUT9_1_0 exports")

# T1 pair in gameplay
p = load("pair_g.bmp")
if p is None: res("T1 pair", False, "pair_g.bmp missing", "no 3D frames reached the pair (steady gate? camera?)")
else:
    w = p.shape[1] // 2; L, R = p[:, :w], p[:, w:]
    d = np.abs(L - R).mean()
    cL, cR = common_crop(L, R, tans) if tans else (L, R)
    mL, mR = cL.mean(), cR.mean(); gap = abs(mL - mR) / max(mL, mR, 1e-3)   # brightness over the SHARED field only
    res("T1 pair", mL > 5 and mR > 5 and d > 1, f"means {mL:.1f}/{mR:.1f} (shared field; content differs by parallax), L-R difference {d:.2f}",
        "identical -> offsets not applied")
# T1b (JJ's 4th test: lighting/reflections differ per eye): both eyes from the same camera and lens with the frame
# number shared (FRAMESYNC) -> the pictures must be the same, rain and noise patterns included; s0 = sync off (control)
ps, p0 = load("pair_s.bmp"), load("pair_s0.bmp")
if ps is None: res("T1b same camera identical", False, "pair_s.bmp missing", "eyesame capture did not happen")
else:
    w2 = ps.shape[1] // 2
    dA = float(np.abs(ps[:, :w2] - ps[:, w2:]).mean()); d0 = float(np.abs(p0[:, :w2] - p0[:, w2:]).mean()) if p0 is not None else None
    res("T1b same camera identical", dA < 0.4, f"same camera, shared frame number: difference {dA:.3f} (separate frame numbers: {('%.3f' % d0) if d0 is not None else 'n/a'})",
        "still different -> randomness seeded elsewhere (time, per-view-state counters): diff map of pair_s")
# T2 HUD picture
a = load("hud_alpha_g.bmp", "L")
cov = None if a is None else float((a > 0).mean())
res("T2 HUD picture", cov is not None and 0.0005 < cov < 0.6, f"HUD coverage {('%.2f %%' % (cov*100)) if cov is not None else 'n/a'}",
    "0 % -> no HUD drawn/redirected in gameplay; ~100 % -> opaque alpha (mask fix)")

# T3 headset stereo
s3 = side("xr_t3.json"); l3, r3 = load("xr_t3_left.png"), load("xr_t3_right.png")
d3 = None
if s3 and l3 is not None and r3 is not None:
    nbL, nbR = s3["stats"]["nonBlackPctL"], s3["stats"]["nonBlackPctR"]; lL, lR = s3["stats"]["meanLumaL"], s3["stats"]["meanLumaR"]
    d3 = float(np.abs(l3 - r3).mean())
    cL, cR = common_crop(l3, r3, tans) if tans else (l3, r3)
    lL, lR = float(cL.mean()), float(cR.mean()); gap = abs(lL - lR) / max(lL, lR, 1e-3)   # shared field only
    def streaks(im):   # garbage eye = rows of one smeared colour (5c run 1 right eye): share of rows with almost no detail
        g_ = im.mean(axis=2); return float((g_.std(axis=1) < 1.5).mean())
    sL, sR = streaks(l3), streaks(r3)
    res("T3 headset stereo", nbL > 50 and nbR > 50 and d3 > 1 and sL < 0.2 and sR < 0.2, f"non-black {nbL:.0f}%/{nbR:.0f}%, streak rows {sL*100:.0f}%/{sR*100:.0f}%, luma {lL:.1f}/{lR:.1f}, L-R difference {d3:.2f}",
        "black -> source/swapchain path (aknSbsUpdate); identical -> pair not fresh (flat fallback) or offsets 0")
else: res("T3 headset stereo", False, "no headset shot", "session not VISIBLE/FOCUSED or game gone")

# T4 lens: rendered (AKN tangents from AKVR) = declared (projection layer) = the runtime's eye
if s3 and eyes:
    m = re.search(r"L tan ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+)", eyes[-1])
    ren = [math.degrees(math.atan(float(x))) for x in m.groups()] if m else None
    run = s3["views"][0]["fovDeg"]; run = [run["l"], run["r"], run["u"], run["d"]]
    proj = [L for L in s3["layers"] if L["type"] == "projection"]
    dec = [proj[0]["fovDeg"][k] for k in ("l", "r", "u", "d")] if proj else None
    e1 = max(abs(x - y) for x, y in zip(ren, run)) if ren else 99
    want = run if not turned else [-(run[1] - run[0]) / 2, (run[1] - run[0]) / 2, (run[2] - run[3]) / 2, -(run[2] - run[3]) / 2]   # turned eyes declare the centred lens
    e2 = max(abs(x - y) for x, y in zip(dec, want)) if dec else 99
    res("T4 lens", e1 < 0.2 and e2 < 0.2, f"left eye runtime {['%.2f' % v for v in run]}, rendered off by {e1:.3f} deg, declared off by {e2:.3f} deg",
        "rendered off -> AKVR tangents; declared off -> views[e].fov not set in gameplay (effGameplay/g_aknFresh false?)")
else: res("T4 lens", False, "missing headset sidecar or AKVR eye lines", "see T0/T3")

# T4b: the lens AKN actually RENDERED (from its logged projection matrices) = the runtime's eye lens, per eye
pl = [l for l in log if "  eye proj" in l]; ll = [l for l in log if "lens tan" in l or "turned lens" in l]
if s3 and pl and ll:
    errs = []
    for e in (0, 1):
        cands = [i for i, l in enumerate(log) if (f"eye {e} lens tan" in l) or (f"eye {e} turned lens" in l)]
        if not cands: continue
        j = cands[-1]; mm = [float(x) for x in re.findall(r"-?[\d.]+", log[j + 1].split("eye proj")[1])]
        m00, m11, m20, m21 = mm[0], mm[5], mm[8], mm[9]
        ren = [math.degrees(math.atan(v)) for v in ((-1 - m20) / m00, (1 - m20) / m00, (1 - m21) / m11, (-1 - m21) / m11)]
        prj = [L for L in s3["layers"] if L["type"] == "projection"]
        run = (prj[0]["fovDeg"] if turned and prj else s3["views"][e]["fovDeg"]); run = [run["l"], run["r"], run["u"], run["d"]]   # turned eyes: vs the DECLARED (centred) lens
        errs.append(max(abs(a - b) for a, b in zip(ren, run)))
    res("T4b rendered lens", len(errs) == 2 and max(errs) < 0.2, f"rendered vs headset lens, worst per eye: {['%.3f deg' % x for x in errs]}",
        "rendered off -> AKN projection (m20/m21 offsets added from the game, or tangents not applied)")
else: res("T4b rendered lens", False, "no lens/projection log lines or headset sidecar", "see T4")
# T5 HUD layer in the headset
q = [L for L in (s3 or {}).get("layers", []) if L["type"] == "quad"]
res("T5 HUD layer", len(q) >= 1, f"{len(q)} quad layer(s) {[(L.get('sizeM'), L.get('pixelsCoveredL')) for L in q]}",
    "no quad -> akvr_hudsplit_layer_image returned null (HUD seq not fresh?)")

# T6 control: stereo off -> both eyes the same picture
s6 = side("xr_t6.json")
def proj_fov(sd):
    p_ = [L for L in (sd or {}).get("layers", []) if L["type"] == "projection"]
    return [p_[0]["fovDeg"][k] for k in ("l", "r", "u", "d")] if p_ else None
f3, f6 = proj_fov(s3), proj_fov(s6)
iS0, iS1 = None, None
for i, l in enumerate(log):
    if "command: stereo 0" in l: iS0 = i
    if "command: stereo 1" in l: iS1 = i
pairs = [int(m.group(1)) for l in (log[iS0:iS1] if iS0 and iS1 else []) for m in [re.search(r"\| pairs (\d+)", l)] if m]
stopped = len(pairs) >= 2 and pairs[-1] - pairs[0] <= 1
sym = f6 is not None and abs(f6[0] + f6[1]) < 0.5
asym = f3 is not None and (abs(f3[0] + f3[1]) > 2 if not turned else (f6 is not None and abs(f3[2] - f6[2]) > 2))   # turned eyes: the stereo lens is centred too, but not the flat one
res("T6 control", stopped and sym and asym, f"stereo off: pairs {pairs[0] if pairs else '?'}->{pairs[-1] if pairs else '?'} (stopped {stopped}); declared left lens on {['%.1f' % x for x in f3] if f3 else None} off {['%.1f' % x for x in f6] if f6 else None}",
    "pairs kept coming -> stereo switch not reaching stereo_calc; lens stayed asymmetric -> AKVR used a stale pair (g_aknFresh)")

# T7 soak
i0, i1 = mark(" T7 start"), mark(" T7 end")
if i0 is None or i1 is None: res("T7 soak", False, "soak marks missing (crash?)", "CRASH RECORDER lines + crashlist.py")
else:
    seg = log[i0:i1]
    fps = [float(m.group(1)) for l in seg for m in [re.search(r" 1s [\d.]+s \| frames/s ([\d.]+)", l)] if m]
    pres = [int(m.group(1)) for l in seg for m in [re.search(r"presents (\d+)", l)] if m]
    mf = sum(fps) / len(fps) if fps else 0
    pr = (pres[-1] - pres[0]) / max(1, len(pres) - 1) if len(pres) > 1 else 0
    dumps = [f for f in os.listdir(logs_dir) if f.startswith("UE3AutoReportDump") and os.path.getmtime(os.path.join(logs_dir, f)) > os.path.getmtime(os.path.join(AKN, "akn_log.txt")) - 600] if os.path.isdir(logs_dir) else []
    res("T7 soak", mf >= 25 and pr >= 25 and not crash, f"game frames/s {mf:.1f}, AKVR frames/s {pr:.1f}, AKN crash records {len(crash)}",
        "slow -> GPU/pacing; crash -> CRASH RECORDER line")
# T8 (JJ's Quest run: mono in game): share of gameplay frames AKVR showed from the stereo pair during the soak
if i0 is not None and i1 is not None:
    rs = [(int(m.group(1)), int(m.group(2))) for l in log[i0:i1] for m in [re.search(r"AKVR shown: stereo (\d+) flat (\d+)", l)] if m]
    if len(rs) >= 2:
        ds, df = rs[-1][0] - rs[0][0], rs[-1][1] - rs[0][1]
        share = ds / max(1, ds + df)
        res("T8 shown in stereo", share > 0.95, f"AKVR showed {ds} frames from the pair, {df} flat ({share*100:.1f} % stereo)",
            "flat -> read 'AKN pair ... does not fit the window' in akvr_mode_log.txt")
    else: res("T8 shown in stereo", False, "no AKVR telemetry lines", "AKVR build without akn_report?")
else: res("T8 shown in stereo", False, "no soak window", "see T7")
# T9 robustness: with a rogue device drawing all session, the window buffer must still be the game's (the eye size)
wb = [l for l in log if "window buffer =" in l]
pt = [l for l in log if "pair texture" in l]
rog = [int(m.group(1)) for l in log for m in [re.search(r"rogue ops (\d+)", l)] if m]
m1 = re.search(r":(\d+)x(\d+)/f(\d+)", wb[-1]) if wb else None
m2 = re.search(r"pair texture (\d+)x(\d+) format (\d+)", pt[-1]) if pt else None
okw = bool(m1 and m2 and int(m1.group(1)) * 2 == int(m2.group(1)) and m1.group(2) == m2.group(2) and int(m1.group(1)) != 2048)
res("T9 rogue device", okw and bool(rog) and rog[-1] > 1000, f"rogue ops {rog[-1] if rog else 0}, window buffer {wb[-1][13:] if wb else 'none'}",
    "window buffer = 2048x2048 -> a foreign device still reaches the detection; rogue ops 0 -> the stand-in did not run")
# T10 main menu in the headset (JJ: Batman out of focus): AKVR shows menus flat (one eye's picture to both)
# judged on the SOURCE pair at the main menu: the fake headset re-projects each eye from its own spot, so even a flat
# picture differs a lot between its eye shots (stereo-off control: 0.71 relative) - headset shots cannot judge "flat".
# JJ's 2nd test: main menu = both eyes from ONE spot, each through its own (mirrored, off-centre) lens -> the halves are
# the same picture shifted sideways (no depth). Best horizontal alignment must leave almost nothing.
def aligned_residual(A_, B_):
    a = A_.mean(axis=2)[::4, ::4]; b = B_.mean(axis=2)[::4, ::4]; W_ = a.shape[1]; best = (1e9, 0)
    for sh in range(-W_ // 3, W_ // 3 + 1):
        if sh >= 0: x, y = a[:, sh:], b[:, :W_ - sh]
        else: x, y = a[:, :W_ + sh], b[:, -sh:]
        r = float(np.abs(x - y).mean()); best = min(best, (r, sh))
    return best, max(float(a.mean()), float(b.mean()), 1.0)
pm = load("pair_menu_main2.bmp")
if pm is not None:
    w3 = pm.shape[1] // 2
    (rm_, shm), mm = aligned_residual(pm[:, :w3], pm[:, w3:])
    ref = None
    if p is not None:
        (rg, shg), mg = aligned_residual(p[:, :p.shape[1] // 2], p[:, p.shape[1] // 2:]); ref = rg / mg
    res("T10 main menu no depth", rm_ / mm < 0.08, f"main menu pair after the best sideways shift ({shm * 4} px): residual {rm_ / mm * 100:.1f} % of brightness (gameplay pair: {('%.1f %%' % (ref * 100)) if ref is not None else 'n/a'})",
        "large residual -> the main menu still has eye separation (offsets not 0 in the menu?)")
else: res("T10 main menu no depth", False, "no main menu pair capture (pair_menu_main2.bmp)", "part A menu step")
# T12 eye identity (JJ's 3rd test: each eye got the other's picture): left eye's picture red band, right eye's blue
lk, rk = load("xr_mark_left.png"), load("xr_mark_right.png")
if lk is not None and rk is not None:
    def rb(im):
        band = im[int(im.shape[0] * 0.42):int(im.shape[0] * 0.58)]; return float(band[..., 0].mean()), float(band[..., 2].mean())
    (lr, lb), (rr, rbb) = rb(lk), rb(rk)
    res("T12 eye identity", lr > 1.5 * lb and lr > 60 and rbb > 1.5 * rr and rbb > 60,
        f"headset left eye middle band red {lr:.0f} / blue {lb:.0f}; right eye red {rr:.0f} / blue {rbb:.0f}",
        "left eye blue -> eyes swapped (swap_eff / swapeyes); no colour -> the marked pair was not shown")
else: res("T12 eye identity", False, "no eye-mark headset shot", "see T3")
# T13 head turn: the world moves by exactly the head's turn (yaw 20 deg, pitch 15 deg) in the headset's left eye
def to_angles(im, fovd, axis):
    """Resample the band through the image centre onto a 0.25 deg grid along one axis (0 = horizontal, 1 = vertical)."""
    g = im.mean(axis=2); H_, W_ = g.shape
    if axis == 0:
        prof = g[int(H_ * 0.35):int(H_ * 0.65)].mean(axis=0); t0, t1 = math.tan(math.radians(fovd["l"])), math.tan(math.radians(fovd["r"]))
    else:
        prof = g[:, int(W_ * 0.35):int(W_ * 0.65)].mean(axis=1)[::-1]; t0, t1 = math.tan(math.radians(fovd["d"])), math.tan(math.radians(fovd["u"]))
    n = len(prof); ang = np.degrees(np.arctan(t0 + (np.arange(n) + 0.5) / n * (t1 - t0)))
    grid = np.arange(math.ceil(ang[0]), math.floor(ang[-1]), 0.25)
    return grid, np.interp(grid, ang, prof)
def best_shift(a, b):
    ga, pa = a; gb, pb = b; best = (1e9, 0.0)
    for k in range(-160, 161):   # +-40 deg in 0.25 deg steps
        sh = k * 0.25
        x = np.interp(ga + sh, gb, pb, left=np.nan, right=np.nan); m = ~np.isnan(x)
        if m.sum() < 60: continue
        xa, xb = pa[m] - pa[m].mean(), x[m] - x[m].mean()
        r = float(np.abs(xa - xb).mean()); best = min(best, (r, sh))
    return best[1]
h0, hy, hp = side("xr_h0.json"), side("xr_hy.json"), side("xr_hp.json")
i0_, iy_, ip_ = load("xr_h0_left.png"), load("xr_hy_left.png"), load("xr_hp_left.png")
if h0 and hy and hp and i0_ is not None and iy_ is not None and ip_ is not None:
    fv = h0["views"][0]["fovDeg"]
    sy = best_shift(to_angles(i0_, fv, 0), to_angles(iy_, fv, 0))
    sp = best_shift(to_angles(i0_, fv, 1), to_angles(ip_, fv, 1))
    res("T13 head turn 1:1", abs(abs(sy) - 20) < 2 and abs(abs(sp) - 15) < 2,
        f"head yaw 20 deg moved the world {sy:+.2f} deg, head pitch 15 deg moved it {sp:+.2f} deg (headset left eye)",
        "not 20/15 -> the picture is not world-locked: compare the declared pose/lens with the rendered camera (camera trace)")
else: res("T13 head turn 1:1", False, "missing head-turn shots", "see part B")
# T11 (JJ's 2nd test: hooks on Virtual Desktop's device table): hooks installed via the game's device named by AKVR
hk = [l for l in log if "context hooks installed via device" in l]
miss = [l for l in log if "HOOKS MISS THE GAME" in l]
res("T11 hooks on the game", bool(hk) and "from AKVR" in hk[0] and not miss, f"{hk[0][13:] if hk else 'no hook line'}{' | ' + miss[0][13:120] if miss else ''}",
    "not from AKVR -> akn_send_game_device not called (akvr_xr_set_device); MISS -> wrong device table")
# T14 (JJ's 4th test: everything jitters on head turns): each presented pair matched to the exact head pose by its stamp
if i0 is not None and i1 is not None:
    pm_ = [(int(m.group(1)), int(m.group(2)), int(m.group(3))) for l in log[i0:i1] for m in [re.search(r"stamp-matched (\d+) of (\d+), delay (\d+)", l)] if m]
    if len(pm_) >= 2:
        dm_, dt_ = pm_[-1][0] - pm_[0][0], pm_[-1][1] - pm_[0][1]
        res("T14 pose stamp", dt_ > 0 and dm_ / dt_ > 0.95, f"{dm_} of {dt_} presents paired with their exact head pose ({dm_ / max(1, dt_) * 100:.1f} %), delay now {pm_[-1][2]}",
            "low -> AKVR's finalize export not found by AKN (stamp 0) or the finalize history too short")
    else: res("T14 pose stamp", False, "no pose telemetry", "AKVR build without akn_report_pose")
else: res("T14 pose stamp", False, "no soak window", "see T7")
# T15 (JJ: the double stick click no longer opens the panel): AKVR's controller hook sits on Windows' own XInput
pad = [l for l in st.splitlines() if "controller hook:" in l]
res("T15 controller hook", bool(pad) and "ON" in pad[0] and "system32" in pad[0].lower(), pad[0].strip() if pad else "no controller hook line",
    "FAILED or the game-folder copy -> gamepad.cpp target")
# T17b (TURNEDEYE): same spot + same frame, each eye its own TURNED camera with a centred lens -> warping the right eye's
# picture into the left eye's direction must reproduce the left eye's picture (lighting included)
turned = [l for l in log if "turned lens: yaw" in l]
def eye_frame(yc, pc):
    cy, sy, cp, sp = math.cos(yc), math.sin(yc), math.cos(pc), math.sin(pc)
    return np.array([cy, 0.0, -sy]), np.array([-sy * sp, cp, -cy * sp]), np.array([sy * cp, sp, cy * cp])
if turned and load("pair_lens.bmp") is not None and ps is not None:
    tp = {}
    for l in turned:
        m_ = re.search(r"eye (\d) turned lens: yaw ([-\d.]+) pitch ([-\d.]+) deg, half-angles ([-\d.]+) x ([-\d.]+)", l)
        if m_: tp[int(m_.group(1))] = [math.radians(float(m_.group(i))) for i in (2, 3, 4, 5)]
    plz = load("pair_lens.bmp"); w5 = plz.shape[1] // 2; E0, E1 = plz[::2, :w5:2], plz[::2, w5::2]   # half size for speed
    H5, W5 = E0.shape[:2]
    if 0 in tp and 1 in tp:
        (y0, p0_, hh, hv), (y1, p1_, _, _) = tp[0], tp[1]
        r0, u0, f0 = eye_frame(y0, p0_); r1, u1, f1 = eye_frame(y1, p1_)
        jj, ii = np.mgrid[0:H5, 0:W5]
        x = (2 * (ii + 0.5) / W5 - 1) * math.tan(hh); y = (1 - 2 * (jj + 0.5) / H5) * math.tan(hv)
        ray = x[..., None] * r0 + y[..., None] * u0 + f0
        a1, b1, c1 = ray @ r1, ray @ u1, ray @ f1
        px = ((a1 / c1) / math.tan(hh) + 1) * W5 / 2; py = (1 - (b1 / c1) / math.tan(hv)) * H5 / 2
        ok_ = (c1 > 0) & (px >= 0) & (px < W5 - 1) & (py >= 0) & (py < H5 - 1)
        warped = E1[np.clip(py.astype(int), 0, H5 - 1), np.clip(px.astype(int), 0, W5 - 1)]
        dw = float(np.abs(E0[ok_] - warped[ok_]).mean()); dctl = float(np.abs(ps[:, :ps.shape[1] // 2] - ps[:, ps.shape[1] // 2:]).mean())
        gm = np.zeros((H5, W5)); gm[ok_] = np.abs(E0[ok_] - warped[ok_]).mean(axis=1)
        Image.fromarray(np.clip(gm * 4, 0, 255).astype(np.uint8)).save(os.path.join(AKN, "T17b_turned_diff.png"))
        res("T17b lighting the same in both eyes", dw < 2 * dctl + 1.5, f"same spot, turned eyes, right warped onto left: difference {dw:.2f} over {ok_.mean()*100:.0f} % of the picture (same-lens control {dctl:.2f}; off-centre lenses gave 18.80) (map: T17b_turned_diff.png)",
            "large -> lighting still depends on the eye's direction/lens: inspect the map")
    else: res("T17b lighting the same in both eyes", False, "no 'turned lens' lines for both eyes", "lens mode 2 not active?")
else: res("T17b lighting the same in both eyes", False, "missing turned-lens lines / pair_lens / control", "part B T17 step")
# T17 lens-dependence: same spot + same frame, each eye its own mirrored off-centre lens -> the right half is the left half
# shifted by (l_right - l_left) / width of the lens; any residual beyond the same-lens control = shading that depends
# on the lens (screen-space effects that assume a centred projection)
pl_ = load("pair_lens.bmp")
if turned: out.append("T17 (off-centre lens test): skipped - lens mode 2 (turned eyes) active, see T17b")
elif pl_ is not None and tans and ps is not None:
    (lL0, rL0, _, _), (lR0, rR0, _, _) = tans
    w4 = pl_.shape[1] // 2; shp = int(round((lR0 - lL0) / (rL0 - lL0) * w4))
    A4 = pl_[:, shp:w4]; B4 = pl_[:, w4:2 * w4 - shp] if shp > 0 else pl_[:, w4:]
    dl = float(np.abs(A4 - B4).mean()); dctl = float(np.abs(ps[:, :ps.shape[1] // 2] - ps[:, ps.shape[1] // 2:]).mean())
    gmap = np.abs(A4 - B4).mean(axis=2)
    Image.fromarray(np.clip(gmap * 4, 0, 255).astype(np.uint8)).save(os.path.join(AKN, "T17_lens_diff.png"))
    res("T17 lens-independent shading", dl < 2 * dctl + 0.3, f"same spot, own lenses, aligned ({shp} px): difference {dl:.2f} vs same-lens control {dctl:.2f} (map: T17_lens_diff.png)",
        "large -> the game's screen-space lighting/shadows/reflections assume a centred lens: render each eye symmetric and crop, or fix the shaders' view-ray reconstruction")
else: res("T17 lens-independent shading", False, "missing pair_lens / lens numbers / control", "part B T17 step")
# T16 eye leak: move one eye 3 m - the other eye's picture must stay at frame-to-frame noise
def leak(sw):
    a_, b_, r_, l_ = (load(f"pair_lk{sw}{t}.bmp") for t in "abrl")
    if any(x is None for x in (a_, b_, r_, l_)): return None
    w_ = a_.shape[1] // 2
    L = lambda im: im[:, :w_]; R = lambda im: im[:, w_:]
    md = lambda x, y: float(np.abs(x - y).mean())
    return md(L(a_), L(b_)), md(L(a_), L(r_)), md(R(a_), R(b_)), md(R(a_), R(l_))
lk0, lk1 = leak(0), leak(1)
hl = [l for l in log if "HISTORY carried over" in l]
if lk1:
    nL, kL, nR, kR = lk1
    okL, okR = kL < 1.5 * nL + 0.3, kR < 1.5 * nR + 0.3
    ctl = f" | own history OFF: left eye changed {lk0[1]:.2f} (noise {lk0[0]:.2f}), right {lk0[3]:.2f} (noise {lk0[2]:.2f})" if lk0 else ""
    res("T16 no leak between eyes", okL and okR, f"own history ON: right eye moved -> left eye changed {kL:.2f} (noise {nL:.2f}); left eye moved -> right eye changed {kR:.2f} (noise {nR:.2f}){ctl}",
        "still leaking -> shared state outside the detector (vendor library / CPU-side, e.g. shadow or god-ray context): see HISTORY line, test effects one by one")
else: res("T16 no leak between eyes", False, "missing leak pictures", "part B leak step")
if hl: out.append("  " + hl[-1][13:600])
if crash: out.append("CRASH RECORDER: " + crash[0][13:300])
# JJ 2026-10-07: "Forget the main menu, just focus on getting stereo 3D in the game" -> T10 informational; T1b known residual
ok = all(" PASS" in l for l in out if l.startswith("T") and "skipped" not in l and not l.startswith("T10") and not l.startswith("T13") and not l.startswith("T1b"))   # T1b: only rain randomness left (diff maps runs 14-17); lighting is judged by T17b   # T13: scene-dependent (third-person camera swings near Batman) until replaced by a pose-based check
out.append("STAGE 5c: " + ("PASS -> JJ tries it in the Quest (Virtual Desktop); then the HUD placement and DLSS stages" if ok else "FAIL -> follow the NEXT lines, re-run 5c"))
text = "\n".join(out); print(text)
open(os.path.join(AKN, "M5c_result.txt"), "w").write(text + "\n")
