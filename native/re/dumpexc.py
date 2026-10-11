"""Exception address + module and the faulting thread's stack (return-address scan) from an Unreal .dmp.
Usage: python dumpexc.py <file.dmp>"""
import sys, struct
from minidump.minidumpfile import MinidumpFile

mf = MinidumpFile.parse(sys.argv[1])
mods = sorted((m.baseaddress, m.baseaddress + m.size, m.name.split('\\')[-1]) for m in mf.modules.modules)
def where(a):
    for lo, hi, n in mods:
        if lo <= a < hi: return f"{n}+{a - lo:X}"
    return "?"
exc = mf.exception.exception_records[0] if mf.exception else None
if exc:
    rec = exc.ExceptionRecord
    print(f"exception code {rec.ExceptionCode} at {rec.ExceptionAddress:X} = {where(rec.ExceptionAddress)}  info {list(rec.ExceptionInformation)[:2]}")
    tid = exc.ThreadId
    reader = mf.get_reader()
    th = [t for t in mf.threads.threads if t.ThreadId == tid][0]
    ctx = th.ContextObject
    rsp = ctx.Rsp if ctx else None
    print(f"thread {tid} rip {ctx.Rip:X} = {where(ctx.Rip)}  rsp {rsp:X}" if ctx else "no context")
    for r in ("Rax", "Rbx", "Rcx", "Rdx", "Rsi", "Rdi", "R8", "R9"):
        print(f"  {r} {getattr(ctx, r):X} {where(getattr(ctx, r))}")
    if rsp:
        try:
            data = reader.read(rsp, 0x600)
            n = 0
            for k in range(0, len(data) - 7, 8):
                v = struct.unpack_from('<Q', data, k)[0]
                w = where(v)
                if w != "?" and ('BatmanAK' in w or 'XINPUT' in w.upper() or 'dinput8' in w or 'd3d11' in w):
                    print(f"  [rsp+{k:X}] {v:X} {w}"); n += 1
                    if n > 25: break
        except Exception as e:
            print("stack read failed:", e)
