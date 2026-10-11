"""AKN stage 5m judge: rain step constants (which rows follow the head), rain view switches, rain pause."""
import sys, os, re
import numpy as np
from PIL import Image
AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
log = open(os.path.join(AKN, "akn_log.txt"), encoding="utf-8", errors="replace").read().splitlines()
out = []
dumps = []; cur = None
for l in log:
    if "RAINCB: rain simulation" in l: cur = {"head": l[13:].strip(), "rows": {}}; dumps.append(cur)
    m = re.search(r"cb0\[(\d+)\] (\S+) (\S+) (\S+) (\S+)", l)
    if m and cur is not None: cur["rows"][int(m.group(1))] = [float(x) for x in m.groups()[1:]]
for d in dumps: out.append(d["head"])
if len(dumps) >= 3:
    a, b, c = dumps[0]["rows"], dumps[1]["rows"], dumps[2]["rows"]
    for r in sorted(a):
        va, vb, vc = np.array(a[r]), np.array(b.get(r, a[r])), np.array(c.get(r, a[r]))
        ch = np.abs(vb - va).max() > 1e-3 or np.abs(vc - va).max() > 1e-3
        out.append(f"cb0[{r}] head0 {np.round(va, 3).tolist()} yaw20 {np.round(vb, 3).tolist()} pitch15 {np.round(vc, 3).tolist()}{'  <- changes with the head' if ch else ''}")
def ld(t):
    p = os.path.join(AKN, f"pair_{t}.bmp")
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float32) if os.path.exists(p) else None
def L(a): return a[:, :a.shape[1] // 2]
def streak(im):
    g = im.mean(axis=2); hp = g - (np.roll(g, 2, 1) + np.roll(g, -2, 1)) / 2; return round(float((hp > 12).mean() * 100), 2)
v0, v1, v2 = ld("rv0"), ld("rv1"), ld("rv2")
if v0 is not None and v1 is not None and v2 is not None:
    out.append(f"rain view: normal streaks {streak(L(v0))}%, rain hidden {streak(L(v1))}%, only rain {streak(L(v2))}% (mean brightness only-rain {L(v2).mean():.1f} vs normal {L(v0).mean():.1f})")
p0a, p0b, p1a, p1b = ld("rp0a"), ld("rp0b"), ld("rp1a"), ld("rp1b")
if all(x is not None for x in (p0a, p0b, p1a, p1b)):
    out.append(f"only rain, 3 s apart: running {np.abs(L(p0a) - L(p0b)).mean():.3f}, PAUSED {np.abs(L(p1a) - L(p1b)).mean():.3f}")
fl = [l for l in log if "far rain patched" in l]; out.append(fl[-1][13:].strip()[-45:] if fl else "no far-rain telemetry")
crashes = [l for l in log if "CRASH RECORDER" in l and "XINPUT9" in l.upper().split("| RSP")[0]]
out.append(f"AKN crash records: {len(crashes)}")
print("\n".join(out)); open(os.path.join(AKN, "M5m_result.txt"), "w").write("\n".join(out) + "\n")
