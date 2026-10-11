"""HF4 driver (2026-10-09): the normal-gameplay pixel-shader reference for CAPDET (akn\\ps_normal_ref.txt), BLACKKEY on
the pause (pause opened and closed with a log check), and one CAPDET sequence in the pause (end-to-end test).
Usage: python hf4_driver.py <akn folder> <tools folder>"""
import os, sys, time, subprocess, json

AKN, TOOLS = sys.argv[1], sys.argv[2]
CMD = os.path.join(AKN, "command.txt"); LOG = os.path.join(AKN, "akn_log.txt")
XS = os.path.join(os.environ["LOCALAPPDATA"], "AKN", "xrsim")
SHOT = r"D:\Documents\Antigravity\VR_Modding\ac6vr\tools\xrsim-tools\xrsim-shot.ps1"
out = []


def say(t):
    t = time.strftime("%H:%M:%S ") + t; print(t, flush=True); out.append(t)


def cmd(c, wait=1.0):
    with open(CMD, "w") as f: f.write(c + "\n")
    for _ in range(40):
        if not os.path.exists(CMD): break
        time.sleep(0.1)
    time.sleep(wait)


def log_pos(): return os.path.getsize(LOG)


def log_since(p):
    with open(LOG, "rb") as f: f.seek(p); return f.read().decode("utf-8", "replace")


def tool(*a): subprocess.run([sys.executable, os.path.join(TOOLS, "akmenu.py"), *a], capture_output=True)


def xrshot(name):
    r = subprocess.run(["powershell", "-NoProfile", "-Command", f"& '{SHOT}' -Dir '{XS}' -Out '{os.path.join(AKN, name)}' -Quiet -TimeoutSec 20 | ConvertTo-Json -Depth 3"], capture_output=True, text=True)
    try: s = json.loads(r.stdout); say(f"headset shot {name}: luma L {s.get('MeanLumaL')} R {s.get('MeanLumaR')}")
    except Exception: say(f"headset shot {name}: failed {r.stderr[:120]}")


def pause(on):
    want = "skip rules ON" if on else "skip rules off"
    for attempt in range(3):
        p = log_pos(); tool("key", "0x1b")
        for _ in range(25):
            if want in log_since(p): say(f"pause {'open' if on else 'closed'} (try {attempt + 1})"); return True
            time.sleep(0.2)
    say(f"pause {'open' if on else 'closed'}: NOT confirmed"); return False


if __name__ == "__main__":
    # 1. normal reference: union of several inventories while the camera turns
    ref = set()
    for i in range(4):
        inv = os.path.join(AKN, "ps_inventory.txt")
        if os.path.exists(inv): os.remove(inv)
        cmd("psinv 4", 1.0)
        for _ in range(30):
            if os.path.exists(inv): break
            time.sleep(0.2)
        time.sleep(0.3)
        if os.path.exists(inv): ref |= set(l.strip() for l in open(inv) if l.strip())
        tool("mouse", "500", "0"); time.sleep(1.0)
    open(os.path.join(AKN, "ps_normal_ref.txt"), "w").write("\n".join(sorted(ref)) + "\n")
    say(f"normal reference: {len(ref)} pixel shaders")
    
    # 2. pause: black backdrops removed (on / off), and a capture (CAPDET end to end)
    if pause(True):
        time.sleep(2)
        xrshot("xr_hf4pause1"); cmd("blackkey 0", 1.5); xrshot("xr_hf4pause0"); cmd("blackkey 1", 1.5)
        p = log_pos(); cmd("capmk hf4p", 1.0)
        for _ in range(100):
            if "CAPDET hf4p:" in log_since(p) and ("s0" not in log_since(p) or True): pass
            time.sleep(0.2)
        say("CAPDET: " + " | ".join(l[13:150] for l in log_since(p).splitlines() if "CAPDET" in l or "view states saved" in l))
        pause(False)
    cmd("mark HF4 end", 0.5)
    open(os.path.join(AKN, "hf4_result.txt"), "w").write("\n".join(out) + "\n")
    