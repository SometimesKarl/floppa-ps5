"""Attributes 'module+0xoff' samples of system DLLs to the nearest preceding export.
Usage: export_sym.py SAMPLES_FILE MODULE [TOP=15]"""
import collections, struct, sys, bisect
path = {"ntdll.dll": r"C:\Windows\System32\ntdll.dll", "win32u.dll": r"C:\Windows\System32\win32u.dll",
        "VCRUNTIME140.dll": r"C:\Windows\System32\vcruntime140.dll", "KERNEL32.DLL": r"C:\Windows\System32\kernel32.dll",
        "amdvlk64.dll": r"C:\Windows\System32\amdvlk64.dll"}[sys.argv[2]]
d = open(path, "rb").read()
pe = struct.unpack_from("<I", d, 0x3c)[0]
nsec = struct.unpack_from("<H", d, pe + 6)[0]
optsz = struct.unpack_from("<H", d, pe + 20)[0]
opt = pe + 24
exp_rva = struct.unpack_from("<I", d, opt + 112)[0]
secs = []
for i in range(nsec):
    o = opt + optsz + i * 40
    va, vs, raw = struct.unpack_from("<III", d, o + 12)[0], struct.unpack_from("<I", d, o + 8)[0], struct.unpack_from("<I", d, o + 20)[0]
    secs.append((va, vs, raw))
def off(rva):
    for va, vs, raw in secs:
        if va <= rva < va + max(vs, 1): return rva - va + raw
    raise ValueError(hex(rva))
e = off(exp_rva)
nfun, nnam, afun, anam, aord = struct.unpack_from("<IIIII", d, e + 20)
funcs = [struct.unpack_from("<I", d, off(afun) + 4 * i)[0] for i in range(nfun)]
names = {}
for i in range(nnam):
    n_rva = struct.unpack_from("<I", d, off(anam) + 4 * i)[0]
    o = struct.unpack_from("<H", d, off(aord) + 2 * i)[0]
    s = d[off(n_rva):d.index(b"\0", off(n_rva))].decode()
    names[funcs[o]] = s
syms = sorted(names.items())
addrs = [a for a, _ in syms]
hist = collections.Counter()
total = 0
for line in open(sys.argv[1]):
    n, loc = line.split()
    total += int(n)
    if loc.startswith(sys.argv[2] + "+"):
        a = int(loc.split("+")[1], 16)
        i = bisect.bisect_right(addrs, a) - 1
        hist[syms[i][1] if i >= 0 else "?"] += int(n)
for name, n in hist.most_common(int(sys.argv[3]) if len(sys.argv) > 3 else 15):
    print(f"{100 * n / total:5.1f}%  {name}")
