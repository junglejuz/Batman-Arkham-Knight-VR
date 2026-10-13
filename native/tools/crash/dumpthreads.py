import sys, struct, re
from minidump.minidumpfile import MinidumpFile
mf = MinidumpFile.parse(sys.argv[1])
mods = sorted([(m.baseaddress, m.endaddress, re.split(r'[\\/]', m.name)[-1]) for m in mf.modules.modules])
def where(a):
    for b, e, n in mods:
        if b <= a < e: return f"{n}+0x{a-b:x}"
    return None
keys = [k.lower() for k in sys.argv[2].split(',')] if len(sys.argv) > 2 else ['dinput8', 'xinput9', 'd3d11', 'openxr', 'vrclient', 'virtualdesktop', 'nvngx', 'd3dcompiler']
f = open(sys.argv[1], 'rb')
for th in mf.threads.threads:
    st = th.Stack; f.seek(st.Rva); data = f.read(st.DataSize)
    frames = []
    for off in range(0, len(data) - 8, 8):
        w = where(struct.unpack_from('<Q', data, off)[0])
        if w and not w.startswith(('ntdll', 'KERNELBASE', 'kernel32', 'win32u', 'USER32', 'ucrtbase', 'msvcp', 'VCRUNTIME')):
            frames.append(w)
    if any(any(k in fr.lower() for k in keys) for fr in frames):
        print(f"thread {th.ThreadId}: " + " | ".join(frames[:18]))
