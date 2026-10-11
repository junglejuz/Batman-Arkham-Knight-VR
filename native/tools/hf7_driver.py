"""HF7 driver (2026-10-09): crash hunt - JJ crashed "when returning to VR from typing in this window" (02:55, game
routine 0x806870 read a deleted object's drawing data on a worker thread). Focus goes to Notepad (typing) and back to
the game, 8 times; rain counters (RAINPAIR) and frame rate between. Usage: python hf7_driver.py <akn> <tools>"""
import os, sys, re, time, subprocess, ctypes
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hf4_driver as h
import akmenu
u32 = ctypes.windll.user32


def alive():
    return bool(akmenu.game_pids())


def type_keys(text):
    for ch in text:
        vk = u32.VkKeyScanW(ord(ch)) & 0xff
        u32.keybd_event(vk, 0, 0, 0); time.sleep(0.03); u32.keybd_event(vk, 0, 2, 0); time.sleep(0.03)


def game_front():
    hw = akmenu.game_window()
    if not hw: return False
    u32.keybd_event(0x10, 0, 0, 0); u32.keybd_event(0x10, 0, 2, 0)   # shift tap: lets this process change the foreground
    u32.ShowWindow(hw, 9); ok = u32.SetForegroundWindow(hw)
    return bool(ok) and u32.GetForegroundWindow() == hw


def rate(tag):
    ls = h.log_since(max(0, h.log_pos() - 40000)).splitlines()
    f = [l for l in ls if "frames/s" in l][-1:]; r = [l for l in ls if "rain steps/s" in l][-1:]
    h.say(f"{tag}: " + (f[0][13:60] if f else "?") + " | " + (re.sub(r" \| VR panel.*", "", r[0][15:]) if r else "?"))


h.cmd("mark HF7 start", 0.5)
rate("start")
for i in range(8):
    if not alive(): h.say("GAME GONE"); break
    n = subprocess.Popen(["notepad.exe"]); time.sleep(2.5)
    type_keys("typing in another window"); time.sleep(1.0 + (i % 3))
    n.kill(); time.sleep(0.3 + 0.4 * (i % 2))
    h.say(f"round {i + 1}: game in front again {game_front()}")
    time.sleep(3.5); rate(f"round {i + 1}")
    if i == 3: h.tool("mouse", "600", "0"); time.sleep(1); h.xrshot("xr_hf7mid")
h.xrshot("xr_hf7end")
h.cmd("mark HF7 end", 0.5)
open(os.path.join(h.AKN, "hf7_result.txt"), "w").write("\n".join(h.out) + "\n")
