"""AKN stage 5i judge (ONCETIME): same camera + frame number in both eyes, left render with / without zero elapsed time."""
import sys, os, re
import numpy as np
from PIL import Image
AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
def lr(t):
    p = os.path.join(AKN, f"pair_{t}.bmp")
    if not os.path.exists(p): return None
    a = np.asarray(Image.open(p).convert("RGB"), dtype=np.float32); w = a.shape[1] // 2
    d = np.abs(a[:, :w] - a[:, w:]).mean(axis=2); Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(os.path.join(AKN, f"I_{t}_diff.png")); return float(d.mean())
off = [lr("z0a"), lr("z0b")]; on = [lr("z1a"), lr("z1b")]
log = open(os.path.join(AKN, "akn_log.txt"), encoding="utf-8", errors="replace").read().splitlines()
out = [f"time stepped by both renders: same-camera L-R difference {off}", f"left render with zero elapsed time: {on}"]
out += [l[13:].strip()[:900] for l in log if "ONCETIME:" in l or "FAMDUMP" in l][:5]
fps = [float(m.group(1)) for l in log for m in [re.search(r"frames/s ([\d.]+)", l)] if m]
out.append(f"frames/s (last 20 s): {np.mean(fps[-20:]):.1f}" if fps else "no fps")
crashes = [l for l in log if "CRASH RECORDER" in l and "XINPUT9" in l.upper().split("| RSP")[0]]
out.append(f"AKN crash records: {len(crashes)}")
o_ = [x for x in off if x is not None]; n_ = [x for x in on if x is not None]
if o_ and n_: out.append(f"ONCETIME: {'PASS' if max(n_) < 0.4 * min(o_) else 'FAIL'} ({max(n_):.3f} vs {min(o_):.3f}; paused reference 0.020)")
print("\n".join(out)); open(os.path.join(AKN, "M5i_result.txt"), "w").write("\n".join(out) + "\n")
