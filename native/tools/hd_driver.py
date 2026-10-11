"""HD driver (2026-10-09): detective mode (view-state snapshots, the vignette shader hunt) and the wheel backdrop hunt
(pad buttons to open a wheel, then HUD-draw bisection). Drives AKN through akn\\command.txt; run by Run-HD-B.ps1.
Usage: python hd_driver.py <akn folder>"""
import os, re, sys, time, shutil

AKN = sys.argv[1]
CMD = os.path.join(AKN, "command.txt"); LOG = os.path.join(AKN, "akn_log.txt")
out = []


def say(t):
    t = time.strftime("%H:%M:%S ") + t; print(t, flush=True); out.append(t)


def alive():
    return os.system('tasklist /FI "IMAGENAME eq BatmanAK.exe" | find /I "BatmanAK.exe" >nul') == 0


def cmd(c, wait=1.0):
    with open(CMD, "w") as f: f.write(c + "\n")
    for _ in range(40):
        if not os.path.exists(CMD): break
        time.sleep(0.1)
    time.sleep(wait)


def log_size():
    return os.path.getsize(LOG)


def log_since(pos):
    with open(LOG, "rb") as f:
        f.seek(pos); return f.read().decode("utf-8", "replace").splitlines()


def hudstat(n=4):
    """the last HUDSTAT line after asking for n frames: (draws, cells, edge)"""
    pos = log_size(); cmd(f"hudstat {n}", 0.3 + n * 0.05)
    for _ in range(20):
        ls = [l for l in log_since(pos) if "HUDSTAT seq" in l]
        if len(ls) >= n: break
        time.sleep(0.15)
    if not ls: return None
    m = re.search(r"draws (\d+) .*sheet cells (\d+) .*edge alpha (\d+)", ls[-1])
    return (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else None


def pairshot(tag):
    p = os.path.join(AKN, f"pair_{tag}.bmp")
    if os.path.exists(p): os.remove(p)
    cmd(f"pairshot {tag}", 0.5)
    for _ in range(80):
        if os.path.exists(p) and os.path.getsize(p) > 1000:
            s0 = os.path.getsize(p); time.sleep(0.4)
            if os.path.getsize(p) == s0: return True
        time.sleep(0.1)
    return False


def psinv(name):
    inv = os.path.join(AKN, "ps_inventory.txt")
    if os.path.exists(inv): os.remove(inv)
    cmd("psinv 4", 1.0)
    for _ in range(40):
        if os.path.exists(inv): break
        time.sleep(0.2)
    time.sleep(0.3)
    if not os.path.exists(inv): return set()
    dst = os.path.join(AKN, f"ps_{name}.txt"); shutil.copy(inv, dst)
    return set(l.strip() for l in open(dst) if l.strip())


# ---- 1. detective mode
cmd("mark HD detective", 0.5)
cmd("vssnap normal", 1.0); ps_n = psinv("normal"); pairshot("hd_normal")
say(f"normal: {len(ps_n)} pixel shaders")
cmd("padbtn 0001 250", 3.5)   # D-pad up (JJ: detective mode)
cmd("vssnap det", 1.0); ps_d = psinv("det"); pairshot("hd_det")
only = sorted(ps_d - ps_n)
say(f"detective: {len(ps_d)} pixel shaders, {len(only)} only in detective mode: {' '.join(only[:30])}")
for h in only[:16]:
    cmd(f"skipps {h}", 1.0); pairshot(f"hd_detskip_{h[:8]}")
cmd("skipps 0", 1.0)
cmd("padbtn 0001 250", 4.0)   # detective off
cmd("vssnap after", 1.0); pairshot("hd_after"); time.sleep(3); pairshot("hd_after2")
say("detective off: snapshots + pictures taken")

# ---- 2. which pad button opens a wheel (the wheels stop the camera = AKVR's pause look -> "hudskipon 1")
cmd("mark HD wheels", 0.5)
cands = [("RB", 0x0200), ("LB", 0x0100), ("dpad right", 0x0008), ("dpad left", 0x0004), ("dpad down", 0x0002), ("Y", 0x8000)]
wheel = None
for name, mask in cands:
    if not alive(): break
    pos = log_size(); cmd(f"padbtn {mask:04x} 4000", 2.5)
    st = hudstat(4); on = any("skip rules ON" in l for l in log_since(pos))
    say(f"{name}: pause look {on}, sheet {st}")
    time.sleep(2.5)
    if on and st and st[2] > 30 and not wheel: wheel = (name, mask)
say(f"wheel button: {wheel}")

# ---- 3. the wheel's backdrop: bisect its HUD draws (the drawn backdrop = sheet edge alpha > 30)
if wheel and alive():
    cmd(f"padbtn {wheel[1]:04x} 60000", 2.5)
    base = hudstat(4); say(f"wheel open: {base}")
    if base and base[2] > 30:
        lo, hi = 0, max(base[0] - 1, 0)
        while hi > lo and alive():
            mid = (lo + hi) // 2
            cmd(f"hudskipidx {lo} {mid}", 0.6); a = hudstat(4)
            say(f"  drop draws {lo}..{mid}: {a}")
            if a and a[2] < base[2] * 0.5: hi = mid
            else: lo = mid + 1
        cmd(f"hudskipidx {lo} {lo}", 0.8); a = hudstat(4); say(f"  draw {lo} alone dropped: {a}")
        cmd("capmk hdwheelskip", 4)
        cmd("hudskipidx -1 -1", 0.6); cmd("capmk hdwheel", 4)
    cmd("padbtn 0 0", 1.0)
cmd("mark HD end", 0.5)
open(os.path.join(AKN, "hd_result.txt"), "w").write("\n".join(out) + "\n")
