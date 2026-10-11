"""HF8 driver (2026-10-09): after the 03:14 crash (copying view-state +0xB0/+0xC0 from the first stereo frame): the
default build (copy list empty) must reach gameplay; then each field copied alone in gameplay, the crash-prone order
last. Usage: python hf8_driver.py <akn> <tools>"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hf4_driver as h
import akmenu
def alive(): return bool(akmenu.game_pids())
def fps():
    ls = [l for l in h.log_since(max(0, h.log_pos() - 30000)).splitlines() if "frames/s" in l]
    return ls[-1][13:60] if ls else "?"
h.cmd("mark HF8 start", 0.5); h.say("gameplay: " + fps()); h.cmd("pairshot hf8a", 2)
for off in ("b0", "c0"):
    if not alive(): h.say("GAME GONE"); break
    h.cmd("vscopy clear", 0.3); h.cmd(f"vscopy {off} 4", 0.3); time.sleep(6)
    h.say(f"copy +0x{off}: alive {alive()} " + (fps() if alive() else ""))
    if alive(): h.cmd(f"pairshot hf8{off}", 2); h.cmd("vssnap hf8" + off, 1)
if alive(): h.cmd("vscopy clear", 0.5); h.cmd("mark HF8 end", 0.5)
open(os.path.join(h.AKN, "hf8_result.txt"), "w").write("\n".join(h.out) + "\n")
