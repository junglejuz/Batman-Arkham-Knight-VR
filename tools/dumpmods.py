"""Minimal minidump reader: module bases + the exception record, and mapping of a UE3AutoReportDump's
32-bit-truncated addresses to module + RVA. Usage: python dumpmods.py <dump.dmp> [<UE3AutoReportDump.txt>]"""
import struct, sys, re, os

def read(path):
    b = open(path, 'rb').read()
    sig, ver, nstreams, rva_dir = struct.unpack_from('<IIII', b, 0)
    assert sig == 0x504D444D, 'not a minidump'
    mods, exc = [], None
    for i in range(nstreams):
        stype, size, rva = struct.unpack_from('<III', b, rva_dir + 12 * i)
        if stype == 4:   # ModuleListStream
            n = struct.unpack_from('<I', b, rva)[0]
            for k in range(n):
                o = rva + 4 + 108 * k
                base, msize = struct.unpack_from('<QI', b, o)
                name_rva = struct.unpack_from('<I', b, o + 20)[0]
                ln = struct.unpack_from('<I', b, name_rva)[0]
                name = b[name_rva + 4:name_rva + 4 + ln].decode('utf-16le')
                mods.append((base, msize, name))
        elif stype == 6:  # ExceptionStream
            tid = struct.unpack_from('<I', b, rva)[0]
            code, flags, rec, addr = struct.unpack_from('<IIQQ', b, rva + 8)
            nparams = struct.unpack_from('<I', b, rva + 8 + 24)[0]
            params = struct.unpack_from('<%dQ' % 15, b, rva + 8 + 32)[:nparams]
            exc = (tid, code, addr, params)
    return mods, exc

def where(mods, a):
    for base, size, name in mods:
        if base <= a < base + size:
            return os.path.basename(name), a - base
    return None, None

if __name__ == '__main__':
    mods, exc = read(sys.argv[1])
    for base, size, name in mods:
        n = os.path.basename(name).lower()
        if n in ('batmanak.exe', 'dinput8.dll', 'geo11.dll', 'd3d11.dll', 'xinput9_1_0.dll', 'nvwgf2umx.dll', 'dxgi.dll'):
            print('%-18s base %016X size %X  %s' % (os.path.basename(name), base, size, name))
    if exc:
        tid, code, addr, params = exc
        m, r = where(mods, addr)
        print('exception tid %d code %08X at %016X = %s+%X params %s' % (tid, code, addr, m, r or 0, [hex(p) for p in params]))
    if len(sys.argv) > 2:
        rep = open(sys.argv[2], 'rb').read().decode('utf-16le', 'replace')
        for mm in re.finditer(r'Address = 0x([0-9a-f]+) .*?\[in ([^\]]+)\]', rep):
            lo = int(mm.group(1), 16); mod = os.path.basename(mm.group(2)).lower()
            for base, size, name in mods:
                if os.path.basename(name).lower() == mod:
                    full = (base & ~0xFFFFFFFF) | lo
                    if full < base: full += 1 << 32
                    print('  %-14s +%X' % (mod, full - base))
                    break
