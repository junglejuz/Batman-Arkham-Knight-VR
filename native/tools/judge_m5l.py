"""AKN stage 5l judge: recorded (deferred) work seen, rain steps per eye, rain-step-once retest (same camera)."""
import sys, os, re, glob
import numpy as np
from PIL import Image
AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
log = open(os.path.join(AKN, "akn_log.txt"), encoding="utf-8", errors="replace").read().splitlines()
out = [l[13:].strip() for l in log if "deferred context" in l][:3]
out += [l[13:].strip() for l in log if "compute shader" in l and ("a96594b16ceb399b" in l or "bc5c6aebf60c9308" in l)][:6]
out.append(f"compute shaders listed: {len([l for l in log if 'compute shader' in l])}, of them recorded: {len([l for l in log if 'compute shader' in l and 'recorded' in l])}")
rs = [l for l in log if "rain steps/s" in l]; out.append(rs[-1][13:].strip()[:200] if rs else "no rain telemetry")
dl = [l for l in log if "recorded draws" in l]; out.append(dl[-1][13:].strip()[-70:] if dl else "no deferred telemetry")
res = {}
for p in sorted(glob.glob(os.path.join(AKN, "pair_l[013]*.bmp")), key=os.path.getmtime):
    m = int(os.path.basename(p)[6]); a = np.asarray(Image.open(p).convert("RGB"), dtype=np.float32); w = a.shape[1] // 2
    res.setdefault(m, []).append(round(float(np.abs(a[:, :w] - a[:, w:]).mean()), 3))
out.append(f"same camera L-R difference by rain-step-once mode: {res}")
if 0 in res and 3 in res: out.append(f"RAINONCE: {'PASS' if max(res[3]) < 0.4 * min(res[0]) else 'FAIL'}")
b = os.path.join(AKN, "bisect_result.txt")
if os.path.exists(b): out += ["bisection: " + l for l in open(b).read().splitlines()]
crashes = [l for l in log if "CRASH RECORDER" in l and "XINPUT9" in l.upper().split("| RSP")[0]]
out.append(f"AKN crash records: {len(crashes)}")
print("\n".join(out)); open(os.path.join(AKN, "M5l_result.txt"), "w").write("\n".join(out) + "\n")
