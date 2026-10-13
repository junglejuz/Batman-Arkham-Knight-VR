import sys, struct, re
from minidump.minidumpfile import MinidumpFile
mf = MinidumpFile.parse(sys.argv[1]); r = mf.get_reader()
exc = mf.exception.exception_records[0]
er = exc.ExceptionRecord
print("exception", er.ExceptionCode, "at", hex(er.ExceptionAddress), "thread", exc.ThreadId,
      "params", [hex(x) for x in er.ExceptionInformation[:er.NumberParameters]])
mods = sorted([(m.baseaddress, m.endaddress, re.split(r'[\\/]', m.name)[-1]) for m in mf.modules.modules])
def where(a):
    for b, e, n in mods:
        if b <= a < e: return f"{n}+0x{a-b:x}"
    return None
print("fault in:", where(er.ExceptionAddress))
f = open(sys.argv[1], 'rb'); f.seek(exc.ThreadContext.Rva); ctx = f.read(exc.ThreadContext.DataSize)
names = ['rax','rcx','rdx','rbx','rsp','rbp','rsi','rdi','r8','r9','r10','r11','r12','r13','r14','r15']
regs = {n: struct.unpack_from('<Q', ctx, 0x78 + 8 * i)[0] for i, n in enumerate(names)}
rip = struct.unpack_from('<Q', ctx, 0xF8)[0]; rsp = regs['rsp']
print("rip", hex(rip), where(rip), "rsp", hex(rsp))
print(" ".join(f"{k}={v:x}" for k, v in regs.items()))
data = b''
th = [t for t in mf.threads.threads if t.ThreadId == exc.ThreadId][0]
st = th.Stack; base = st.StartOfMemoryRange
f.seek(st.Rva); whole = f.read(st.DataSize)
print("stack range", hex(base), hex(base + len(whole)))
data = whole[max(0, rsp - base):] if base <= rsp < base + len(whole) else whole
n = 0
for off in range(0, len(data) - 8, 8):
    a = struct.unpack_from('<Q', data, off)[0]
    w = where(a)
    if w and (n < 4 or not w.startswith(('ntdll', 'KERNELBASE', 'kernel32'))):
        print(f"  rsp+{off:05x}: {w}"); n += 1
        if n > int(sys.argv[2] if len(sys.argv) > 2 else 60): break
