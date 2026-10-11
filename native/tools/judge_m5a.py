"""AKN stage 5a judge: PASS/FAIL per test from akn_log.txt and the captures (NOTES.md "5a test - measured pass/fail").
Usage: python judge_m5a.py [akn folder]   -> prints the table and writes M5a_result.txt there."""
import re, sys, os
import numpy as np
from PIL import Image

AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
log = open(os.path.join(AKN, "akn_log.txt"), encoding="utf-8", errors="replace").read().splitlines()
out = []
def res(name, ok, detail, nxt):
    out.append(f"{name}: {'PASS' if ok else 'FAIL'} - {detail}" + ("" if ok else f"  -> NEXT: {nxt}"))
def img(name):
    p = os.path.join(AKN, name)
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float32) if os.path.exists(p) else None
def gray(name):
    p = os.path.join(AKN, name)
    return np.asarray(Image.open(p).convert("L"), dtype=np.float32) if os.path.exists(p) else None
def mark_idx(text):
    for i, l in enumerate(log):
        if "MARK" in l and text in l: return i
    return None

# T0 start
hooked = sum(1 for l in log if " hooked " in l)
mism = [l for l in log if "SIGNATURE MISMATCH" in l]
ok0 = hooked >= 9 and not mism and any("D3D11 device" in l for l in log) and any("exports ready" in l for l in log)
res("T0 start", ok0, f"{hooked} hooks, {len(mism)} signature mismatches, device={'yes' if any('D3D11 device' in l for l in log) else 'NO'}, exports={'yes' if any('exports ready' in l for l in log) else 'NO'}",
    "fix the named hook/export before anything else")

# T1 pair
p0 = img("pair_h0.bmp")
if p0 is None:
    res("T1 pair", False, "pair_h0.bmp missing (no capture / no pair texture)", "check 'pair texture' and 'window buffer =' lines")
else:
    w = p0.shape[1] // 2
    L, R = p0[:, :w], p0[:, w:]
    mL, mR, diff = L.mean(), R.mean(), np.abs(L - R).mean()
    bri = abs(mL - mR) / max(mL, mR, 1e-3)
    ok1 = mL > 5 and mR > 5 and diff > 1.0 and bri < 0.08   # gameplay distances only (5a run: main-menu close-up gave 23.6 %)
    res("T1 pair", ok1, f"left mean {mL:.1f}, right mean {mR:.1f}, brightness gap {bri*100:.1f} %, left-right difference {diff:.2f}",
        "identical halves -> eye offset/copy half; black -> window buffer; brightness gap -> shared exposure")

# T2 HUD
a1 = gray("hud_alpha_h1.bmp")
cov = None if a1 is None else float((a1 > 0).mean())
i2, i3 = mark_idx(" T2"), mark_idx(" T3 start")
red = None
if i2 is not None:
    seg = log[i2:(i3 if i3 else len(log))]
    vals = [int(m.group(1)) for l in seg for m in [re.search(r"redirects (\d+)", l)] if m]
    red = (vals[-1] - vals[0]) if len(vals) >= 2 else 0
s0, s1, p1 = img("shot_h0.bmp"), img("shot_h1.bmp"), img("pair_h1.bmp")
d0 = d1 = None
if s0 is not None and p0 is not None and s0.shape[1] * 2 == p0.shape[1]: d0 = float(np.abs(s0 - p0[:, :s0.shape[1]]).mean())
if s1 is not None and p1 is not None and s1.shape[1] * 2 == p1.shape[1]: d1 = float(np.abs(s1 - p1[:, :s1.shape[1]]).mean())
# 2026-10-07 fix: compare INSIDE the HUD-covered pixels - the window with the redirect must not show the HUD picture
hr = img("hud_rgb_h1.bmp")
dh0 = dh1 = None
if a1 is not None and hr is not None and s0 is not None and s1 is not None and s0.shape == hr.shape == s1.shape:
    m = a1 > 0
    if m.any():
        dh0 = float(np.abs(s0[m] - hr[m]).mean()); dh1 = float(np.abs(s1[m] - hr[m]).mean())
ok2 = cov is not None and 0.0005 < cov < 0.60 and (red or 0) > 0 and dh0 is not None and dh1 > dh0 + 10
res("T2 HUD", ok2, f"HUD alpha coverage {('%.2f %%' % (cov*100)) if cov is not None else 'n/a'}, redirects during T2 {red}, in HUD pixels window-vs-HUD difference: redirect off {('%.1f' % dh0) if dh0 is not None else 'n/a'}, redirect on {('%.1f' % dh1) if dh1 is not None else 'n/a'}",
    "redirects 0 -> trace binds after eye 0; coverage ~100% -> opaque HUD alpha (mask fix); window still has HUD -> missed Clear/Copy path")

# T3 soak
i3e = mark_idx(" T3 end")
if i3 is None or i3e is None:
    res("T3 soak", False, "soak marks missing (game gone before the end?)", "crashmap.py on the newest UE3AutoReportDump")
else:
    seg = log[i3:i3e]
    fps = [float(m.group(1)) for l in seg for m in [re.search(r" 1s [\d.]+s \| frames/s ([\d.]+)", l)] if m]
    st = [int(m.group(1)) for l in seg for m in [re.search(r"stereo frames (\d+)", l)] if m]
    faults = [l for l in seg if "FAULT" in l]
    rate = (st[-1] - st[0]) / max(1, len(st) - 1) if len(st) >= 2 else 0
    mf = sum(fps) / len(fps) if fps else 0
    alive = any(" 1s " in l for l in log[i3e:])
    ok3 = alive and not faults and mf >= 28 and rate >= 28
    res("T3 soak", ok3, f"mean frames/s {mf:.1f}, stereo frames/s {rate:.1f}, faults {len(faults)}, alive after {alive}",
        "crash -> crashmap.py; slow -> GPU busy lines")

overall = all(" PASS" in l for l in out)
out.append("STAGE 5a: " + ("PASS -> go to stage 5b (AKVR engine mode)" if overall else "FAIL -> follow the NEXT lines above, re-run 5a only"))
text = "\n".join(out)
print(text)
open(os.path.join(AKN, "M5a_result.txt"), "w").write(text + "\n")
