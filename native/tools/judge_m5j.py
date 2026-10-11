"""AKN stage 5j judge (RTWAIT): same camera + frame number, rain L-R difference with the game thread held until both
eye renders are done, and the frame-rate cost."""
import sys, os, re
import numpy as np
from PIL import Image
AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
def lr(t):
    p = os.path.join(AKN, f"pair_{t}.bmp")
    if not os.path.exists(p): return None
    a = np.asarray(Image.open(p).convert("RGB"), dtype=np.float32); w = a.shape[1] // 2
    d = np.abs(a[:, :w] - a[:, w:]).mean(axis=2); Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(os.path.join(AKN, f"J_{t}_diff.png")); return float(d.mean())
off = [lr("w0a"), lr("w0b")]; on = [lr("w1a"), lr("w1b")]
log = open(os.path.join(AKN, "akn_log.txt"), encoding="utf-8", errors="replace").read().splitlines()
def idx(t):
    for i, l in enumerate(log):
        if "MARK" in l and t in l: return i
def fps(a, b):
    i, j = idx(a), idx(b)
    v = [float(m.group(1)) for l in (log[i:j] if i is not None and j is not None else []) for m in [re.search(r"frames/s ([\d.]+)", l)] if m]
    return np.mean(v[2:]) if len(v) > 3 else None
out = [f"no wait: same-camera L-R difference {off}", f"game thread waits for both renders: {on}",
       f"frames/s no wait {fps('J fps0', 'J fps1')}, with wait {fps('J fps1', 'J end')}"]
w = [l for l in log if "render wait" in l]; out.append(w[-1][13:].strip()[-80:] if w else "no wait telemetry")
crashes = [l for l in log if "CRASH RECORDER" in l and "XINPUT9" in l.upper().split("| RSP")[0]]
out.append(f"AKN crash records: {len(crashes)}")
o_ = [x for x in off if x is not None]; n_ = [x for x in on if x is not None]
if o_ and n_: out.append(f"RACE: {'CONFIRMED' if max(n_) < 0.4 * min(o_) else 'NOT the cause'} ({max(n_):.3f} vs {min(o_):.3f}; paused 0.020)")
print("\n".join(out)); open(os.path.join(AKN, "M5j_result.txt"), "w").write("\n".join(out) + "\n")
