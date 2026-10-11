"""Wait until the game renders steadily again (after a loading screen): N one-second lines in a row with frames/s >= F,
all logged after this script started. Usage: python wait_steady.py [N=5] [F=20] [timeout_s=180]"""
import re, sys, time, os
LOG = r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn\akn_log.txt"
args = [a for a in sys.argv[1:] if not a.startswith('--')]
n_need = int(args[0]) if len(args) > 0 else 5
f_need = float(args[1]) if len(args) > 1 else 20
timeout = float(args[2]) if len(args) > 2 else 180
start = time.time()
pos = os.path.getsize(LOG)
run = 0
need_hitch = '--after-hitch' in sys.argv   # 5c run 2: steady menus looked like gameplay; a load (hitch) must come first
hitched = not need_hitch
while time.time() - start < timeout:
    time.sleep(1)
    with open(LOG, encoding="utf-8", errors="replace") as f:
        f.seek(pos); new = f.read(); pos = f.tell()
    for l in new.splitlines():
        if not hitched and 'hitch ' in l and 'stereo paused' in l: hitched = True; run = 0; continue
        m = re.search(r" 1s [\d.]+s \| frames/s ([\d.]+)", l)
        # LOADSEEN 2026-10-09 (JJ: "why are the button presses being missed?" - they were not: the VR loading screen
        # runs smoothly, so no hitch line came and the script pressed Enter again into the loading screen). The world
        # stops drawing while loading: a one-second line with frames/s below 1 is the load starting too.
        if not hitched and m and float(m.group(1)) < 1.0: hitched = True; run = 0; print(f"load started ({l[:12]})"); continue
        if not hitched: continue
        if m:
            run = run + 1 if float(m.group(1)) >= f_need else 0
            if run >= n_need:
                print(f"steady after {time.time() - start:.0f} s ({l[:40]})"); sys.exit(0)
print(f"not steady within {timeout:.0f} s" + ("" if hitched else " (no loading hitch seen)")); sys.exit(1)
