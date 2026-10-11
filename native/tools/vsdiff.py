"""DETECTIVE view-state diff (2026-10-09): vs_<normal|det|after>_<0 game|1 right eye>.bin (0x8850 bytes each, saved by
AKN "vssnap"). Lists 4-byte fields the GAME's view state changes into detective mode and back out of it, next to what the
right eye's own view state did - the fields stuck in the right eye after the exit are the ones to carry over.
Usage: python vsdiff.py [akn folder]"""
import os, sys, struct

AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"


def load(tag, e):
    b = open(os.path.join(AKN, f"vs_{tag}_{e}.bin"), "rb").read()
    return struct.unpack(f"<{len(b) // 4}I", b[: len(b) // 4 * 4])


g = {t: load(t, 0) for t in ("normal", "det", "after")}
r = {t: load(t, 1) for t in ("normal", "det", "after")}


def f(u):
    v = struct.unpack("<f", struct.pack("<I", u))[0]
    return f"{v:.4g}" if 1e-6 < abs(v) < 1e7 else f"0x{u:08x}"


def ptrlike(u):
    return 0x10000 < u and False


rows = []
for i in range(len(g["normal"])):
    gn, gd, ga = g["normal"][i], g["det"][i], g["after"][i]
    rn, rd, ra = r["normal"][i], r["det"][i], r["after"][i]
    game_toggled = gn != gd and ga != gd
    game_changed = gn != gd or gd != ga
    if not game_changed: continue
    stuck = (rd != rn) and (ra == rd)            # right eye went in, never came out
    never = (rn == rd == ra)                      # right eye never followed
    tag = "STUCK" if stuck else ("never" if never else "")
    rows.append((0 if stuck else (1 if game_toggled else 2), i * 4, tag, gn, gd, ga, rn, rd, ra, game_toggled))
rows.sort()
print(f"{len(rows)} fields the game changed; STUCK = the right eye went into detective mode and stayed")
print(" off    | kind  | game normal / det / after          | right eye normal / det / after")
for k, off, tag, gn, gd, ga, rn, rd, ra, tog in rows[:120]:
    print(f" {off:05X} | {tag:5s}{'T' if tog else ' '}| {f(gn):>10} {f(gd):>10} {f(ga):>10} | {f(rn):>10} {f(rd):>10} {f(ra):>10}")
