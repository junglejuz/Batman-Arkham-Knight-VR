"""AKN stage 5g judge (RANDSYNC): same camera + shared frame number in both eyes; L-R difference per dice mode."""
import sys, os, glob, re
import numpy as np
from PIL import Image
AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
res = {}
for p in sorted(glob.glob(os.path.join(AKN, "pair_gr*-*.bmp")), key=os.path.getmtime):
    m = int(re.search(r"pair_gr(\d)-", p).group(1))
    a = np.asarray(Image.open(p).convert("RGB"), dtype=np.float32); w = a.shape[1] // 2
    d = np.abs(a[:, :w] - a[:, w:]).mean(axis=2); res.setdefault(m, []).append(float(d.mean()))
    Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(p.replace(".bmp", "_diff.png"))
log = open(os.path.join(AKN, "akn_log.txt"), encoding="utf-8", errors="replace").read().splitlines()
out = [f"dice mode {m} ({['none','UE3 seed','C runtime','both'][m]}): same-camera L-R difference {['%.3f' % x for x in v]}" for m, v in sorted(res.items())]
out += [l.strip()[13:] for l in log if "RANDSYNC:" in l][:3]
dl = [l for l in log if "dice shared:" in l]; out.append(dl[-1].strip()[13:] if dl else "no dice telemetry")
crashes = [l for l in log if "CRASH RECORDER" in l and "XINPUT9" in l.upper().split("| RSP")[0]]
out.append(f"AKN crash records: {len(crashes)}")
best = min((min(v), m) for m, v in res.items() if m) if any(m for m in res) else None
if 0 in res and best:
    ok = best[0] < 0.4 * min(res[0])
    out.append(f"RANDSYNC: {'PASS' if ok else 'FAIL'} - best mode {best[1]} {best[0]:.3f} vs none {min(res[0]):.3f}")
print("\n".join(out)); open(os.path.join(AKN, "M5g_result.txt"), "w").write("\n".join(out) + "\n")
