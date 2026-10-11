"""AKN stage 5e judge: same camera + shared frame number in both eyes; rain difference with each eye's own view state
(ea, eb, ee) vs both eyes sharing the game's view state (ec, ed). Usage: python judge_m5e.py [akn folder]"""
import sys, os
import numpy as np
from PIL import Image
AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
def lr(tag):
    p = os.path.join(AKN, f"pair_{tag}.bmp")
    if not os.path.exists(p): return None
    a = np.asarray(Image.open(p).convert("RGB"), dtype=np.float32); w = a.shape[1] // 2
    d = np.abs(a[:, :w] - a[:, w:]).mean(axis=2)
    Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(os.path.join(AKN, f"E_{tag}_diff.png"))
    return float(d.mean())
own = [lr(t) for t in ("ea", "eb", "ee")]; shared = [lr(t) for t in ("ec", "ed")]
o = [x for x in own if x is not None]; s = [x for x in shared if x is not None]
out = [f"E1 own view state per eye: L-R difference {['%.3f' % x for x in o]}",
       f"E2 shared view state:        L-R difference {['%.3f' % x for x in s]}"]
if o and s:
    ok = max(s) < 0.5 * min(o)
    out.append(f"E3 rain follows the view state: {'PASS' if ok else 'FAIL'} -> NEXT: " + (
        "yes -> find the rain's per-view-state fields (diff the two view states, copy them eye to eye)" if ok else
        "no -> the rain changes between the two eye renders by itself (CPU-filled buffer per render or a render-thread counter): trace buffer writes between the renders"))
else: out.append("E3: captures missing")
print("\n".join(out)); open(os.path.join(AKN, "M5e_result.txt"), "w").write("\n".join(out) + "\n")
