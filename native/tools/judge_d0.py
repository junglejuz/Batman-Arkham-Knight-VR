"""DLSS D0 judge: SMAA swap points + motion vectors per eye; frame time at native vs 67 % eye size."""
import os, re
import numpy as np
AKN = r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
out = []
for label in ("native", "67pct", "58pct", "50pct"):
    p = os.path.join(AKN, f"akn_log_D0_{label}.txt")
    if not os.path.exists(p): out.append(f"{label}: no log"); continue
    log = open(p, encoding="utf-8", errors="replace").read().splitlines()
    i = next((k for k, l in enumerate(log) if f"MARK D0 {label} measure" in l), None); j = next((k for k, l in enumerate(log) if f"MARK D0 {label} end" in l), None)
    seg = log[i:j] if i is not None and j is not None else []
    fps = [float(m.group(1)) for l in seg for m in [re.search(r"\| frames/s ([\d.]+)", l)] if m]
    d0 = [tuple(float(x) for x in m.groups()) for l in seg for m in [re.search(r"processor L ([\d.]+) R ([\d.]+), graphics card L ([\d.]+) R ([\d.]+)", l)] if m]
    gb = [float(m.group(1)) for l in seg for m in [re.search(r"GPU busy ([\d.]+)%", l)] if m]
    rd = [float(m.group(1)) for l in seg for m in [re.search(r"Redraw ms/frame ([\d.]+)", l)] if m]
    out.append(f"== {label}: frames/s {np.mean(fps[2:]) if len(fps) > 3 else fps}  | GPU busy {np.mean(gb) if gb else '?'} %")
    if d0:
        a = np.array(d0[2:] if len(d0) > 3 else d0).mean(axis=0)
        out.append(f"   eye render processor ms L {a[0]:.2f} R {a[1]:.2f} (sum {a[0] + a[1]:.2f}) | graphics card ms L {a[2]:.2f} R {a[3]:.2f} (sum {a[2] + a[3]:.2f})")
    if rd: out.append(f"   game thread Redraw ms/frame {np.mean(rd):.2f}")
    out += ["   " + l[13:].strip()[:300] for l in log if l.find("D0 SMAA neighbourhood") >= 0][:1]
    sp = os.path.join(AKN, f"akvr_startup_D0_{label}.txt")
    if os.path.exists(sp): out += ["   " + l.strip() for l in open(sp, encoding="utf-8", errors="replace") if "window client" in l or "one eye renders" in l][:2]
    sm = [l for l in seg if "D0 eye render ms" in l]
    if sm: out.append("   " + sm[-1][13:].strip()[-150:])
print("\n".join(out)); open(os.path.join(AKN, "D0_result.txt"), "w").write("\n".join(out) + "\n")
