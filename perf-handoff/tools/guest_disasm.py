"""Disassemble guest code in the running TEST emulator (the process drive.py launched).

Usage: guest_disasm.py ADDRESS [LENGTH=256] [BEFORE=64]
Reads LENGTH+BEFORE bytes starting BEFORE bytes ahead of ADDRESS from the test process (only if
drive-state.json's recorded identity still matches), wraps them in a minimal ELF64 object and runs
llvm-objdump (VS Build Tools LLVM) with the guest addresses. Read-only: nothing is written into the
process. Disassembly starting mid-instruction may be garbled for the first few lines.
"""
import ctypes as c
import ctypes.wintypes as w
import json
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import owned_process

TOOLS = Path(__file__).resolve().parent
OBJDUMP = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\llvm-objdump.exe"


def elf_with_text(code: bytes) -> bytes:
    shstr = b"\0.text\0.shstrtab\0"
    ehsize, shentsize = 64, 64
    text_off = ehsize
    shstr_off = text_off + len(code)
    sh_off = (shstr_off + len(shstr) + 7) & ~7
    header = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    header += struct.pack("<HHIQQQIHHHHHH", 1, 62, 1, 0, 0, sh_off, 0, ehsize, 0, 0, shentsize, 3, 2)
    sections = bytes(64)
    sections += struct.pack("<IIQQQQIIQQ", 1, 1, 6, 0, text_off, len(code), 0, 0, 16, 0)
    sections += struct.pack("<IIQQQQIIQQ", 7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    body = header + code + shstr
    return body + bytes(sh_off - len(body)) + sections


def main():
    address = int(sys.argv[1], 16)
    length = int(sys.argv[2]) if len(sys.argv) > 2 else 256
    before = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    owned = json.loads((TOOLS / "drive-state.json").read_text()).get("owned")
    if not owned or not owned_process.is_same(owned):
        sys.exit("test process not running")
    k = c.WinDLL("kernel32", use_last_error=True)
    k.OpenProcess.restype = w.HANDLE
    k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
    handle = k.OpenProcess(0x0010 | 0x1000, False, owned["pid"])  # VM_READ | QUERY_LIMITED
    start = address - before
    buf = (c.c_ubyte * (length + before))()
    got = c.c_size_t()
    if not k.ReadProcessMemory(handle, c.c_void_p(start), buf, len(buf), c.byref(got)):
        sys.exit(f"ReadProcessMemory failed: {c.get_last_error()}")
    k.CloseHandle(handle)
    with tempfile.NamedTemporaryFile(suffix=".o", delete=False) as f:
        f.write(elf_with_text(bytes(buf[:got.value])))
        obj = f.name
    out = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", f"--adjust-vma=0x{start:x}", obj],
                         capture_output=True, text=True)
    Path(obj).unlink()
    if "--raw" in sys.argv:
        print(out.stdout)
    lines = [l for l in out.stdout.splitlines() if re.match(r"\s*[0-9a-f]+:", l)]
    for line in lines:
        mark = ">>" if line.strip().startswith(f"{address:x}:") else "  "
        print(mark, line.strip())
    if out.stderr:
        print(out.stderr, file=sys.stderr)


main()
