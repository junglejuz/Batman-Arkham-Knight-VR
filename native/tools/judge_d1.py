"""DLSS D1 judge: frames/s, GPU busy, eye ms per DLSS model; render/output sizes; evaluations."""
import re, os
import numpy as np
AKN = r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
log = open(os.path.join(AKN, "akn_log_D1.txt"), encoding="utf-8", errors="replace").read().splitlines()
out = [l[13:].strip() for l in log if "DLSS: " in l][:6]
def seg(a, b):
    i = next((k for k, l in enumerate(log) if a in l), None); j = next((k for k, l in enumerate(log) if b in l), None)
    return log[i:j] if i is not None and j is not None else []
for pr in (0,):
    s = seg(f"MARK D1 preset {pr} measure", f"MARK D1 preset {pr} end")
    fps = [float(m.group(1)) for l in s for m in [re.search(r"\| frames/s ([\d.]+)", l)] if m]
    gb = [float(m.group(1)) for l in s for m in [re.search(r"GPU busy ([\d.]+)%", l)] if m]
    d0 = [tuple(float(x) for x in m.groups()) for l in s for m in [re.search(r"processor L ([\d.]+) R ([\d.]+), graphics card L ([\d.]+) R ([\d.]+)", l)] if m]
    a = np.array(d0[2:]).mean(axis=0) if len(d0) > 3 else None
    dm = [float(m.group(1)) for l in s for m in [re.search(r"per evaluate ([\d.]+)", l)] if m]
    out.append(f"   preset {pr}: DLSS graphics card ms per eye {np.mean(dm[2:]) if len(dm) > 3 else dm}")
    out.append(f"preset {pr}: frames/s {np.mean(fps[2:]) if len(fps) > 3 else fps} | GPU busy {np.mean(gb) if gb else '?'} % | eye ms processor {a[0]:.2f}/{a[1]:.2f} graphics {a[2]:.2f}/{a[3]:.2f}" if a is not None else f"preset {pr}: no data")
ev = [l for l in log if "DLSS mode" in l]; out.append(ev[-1][13:].strip() if ev else "no DLSS diag")
print("\n".join(out))
sp = os.path.join(AKN, "akvr_startup_D1.txt")
if os.path.exists(sp):
    print("\n".join("AKVR: " + l.strip() for l in open(sp, encoding="utf-8", errors="replace") if any(k in l for k in ("window client", "hud-rect", "swapchain FORCED", "one eye renders", "claiming screen"))))
