"""Stack samples of one thread of a running process, for inclusive (per-caller) costs.

Usage: stack_sample.py PID THREAD_NAME [SAMPLES=400] [INTERVAL_MS=15] [OUT=stacks.txt]
THREAD_NAME matches the thread description exactly ("=Thread_Gpu" style prefix not needed).
Each sample suspends the thread, walks its stack with dbghelp StackWalk64 (x64 unwind data of the
loaded modules) and resumes it. Output: one line per distinct stack, "COUNT addr;addr;..." with
module+offset addresses, innermost first. Symbolize with stack_report.py.
"""
import collections
import ctypes as c
import ctypes.wintypes as w
import sys
import time
from pathlib import Path

k = c.WinDLL("kernel32", use_last_error=True)
dbghelp = c.WinDLL("dbghelp", use_last_error=True)
psapi = c.WinDLL("psapi", use_last_error=True)

PROCESS_QUERY_INFORMATION, PROCESS_VM_READ = 0x0400, 0x0010
THREAD_ACCESS = 0x0002 | 0x0008 | 0x0040  # SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION
CONTEXT_FULL = 0x10000B
IMAGE_FILE_MACHINE_AMD64 = 0x8664


class M128A(c.Structure):
    _fields_ = [("Low", c.c_uint64), ("High", c.c_int64)]


class CONTEXT(c.Structure):
    _fields_ = [("P1Home", c.c_uint64), ("P2Home", c.c_uint64), ("P3Home", c.c_uint64),
                ("P4Home", c.c_uint64), ("P5Home", c.c_uint64), ("P6Home", c.c_uint64),
                ("ContextFlags", w.DWORD), ("MxCsr", w.DWORD), ("SegCs", w.WORD), ("SegDs", w.WORD),
                ("SegEs", w.WORD), ("SegFs", w.WORD), ("SegGs", w.WORD), ("SegSs", w.WORD),
                ("EFlags", w.DWORD), ("Dr0", c.c_uint64), ("Dr1", c.c_uint64), ("Dr2", c.c_uint64),
                ("Dr3", c.c_uint64), ("Dr6", c.c_uint64), ("Dr7", c.c_uint64), ("Rax", c.c_uint64),
                ("Rcx", c.c_uint64), ("Rdx", c.c_uint64), ("Rbx", c.c_uint64), ("Rsp", c.c_uint64),
                ("Rbp", c.c_uint64), ("Rsi", c.c_uint64), ("Rdi", c.c_uint64), ("R8", c.c_uint64),
                ("R9", c.c_uint64), ("R10", c.c_uint64), ("R11", c.c_uint64), ("R12", c.c_uint64),
                ("R13", c.c_uint64), ("R14", c.c_uint64), ("R15", c.c_uint64), ("Rip", c.c_uint64),
                ("FltSave", c.c_byte * 512), ("VectorRegister", M128A * 26),
                ("VectorControl", c.c_uint64), ("DebugControl", c.c_uint64),
                ("LastBranchToRip", c.c_uint64), ("LastBranchFromRip", c.c_uint64),
                ("LastExceptionToRip", c.c_uint64), ("LastExceptionFromRip", c.c_uint64)]


class ADDRESS64(c.Structure):
    _fields_ = [("Offset", c.c_uint64), ("Segment", w.WORD), ("Mode", c.c_int)]


class KDHELP64(c.Structure):
    _fields_ = [("Thread", c.c_uint64), ("ThCallbackStack", w.DWORD),
                ("ThCallbackBStore", w.DWORD), ("NextCallback", w.DWORD),
                ("FramePointer", w.DWORD), ("KiCallUserMode", c.c_uint64),
                ("KeUserCallbackDispatcher", c.c_uint64), ("SystemRangeStart", c.c_uint64),
                ("KiUserExceptionDispatcher", c.c_uint64), ("StackBase", c.c_uint64),
                ("StackLimit", c.c_uint64), ("BuildVersion", w.DWORD),
                ("RetpolineStubFunctionTableSize", w.DWORD),
                ("RetpolineStubFunctionTable", c.c_uint64), ("RetpolineStubOffset", w.DWORD),
                ("RetpolineStubSize", w.DWORD), ("Reserved0", c.c_uint64 * 2)]


class STACKFRAME64(c.Structure):
    _fields_ = [("AddrPC", ADDRESS64), ("AddrReturn", ADDRESS64), ("AddrFrame", ADDRESS64),
                ("AddrStack", ADDRESS64), ("AddrBStore", ADDRESS64),
                ("FuncTableEntry", c.c_void_p), ("Params", c.c_uint64 * 4), ("Far", w.BOOL),
                ("Virtual", w.BOOL), ("Reserved", c.c_uint64 * 3), ("KdHelp", KDHELP64)]


class THREADENTRY32(c.Structure):
    _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ThreadID", w.DWORD),
                ("th32OwnerProcessID", w.DWORD), ("tpBasePri", c.c_long),
                ("tpDeltaPri", c.c_long), ("dwFlags", w.DWORD)]


class MODINFO(c.Structure):
    _fields_ = [("base", c.c_void_p), ("size", w.DWORD), ("entry", c.c_void_p)]


psapi.EnumProcessModulesEx.argtypes = [w.HANDLE, c.POINTER(w.HMODULE), w.DWORD,
                                       c.POINTER(w.DWORD), w.DWORD]
psapi.GetModuleFileNameExW.argtypes = [w.HANDLE, w.HMODULE, w.LPWSTR, w.DWORD]
psapi.GetModuleInformation.argtypes = [w.HANDLE, w.HMODULE, c.POINTER(MODINFO), w.DWORD]
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.SuspendThread.argtypes = [w.HANDLE]
k.ResumeThread.argtypes = [w.HANDLE]
k.GetThreadContext.argtypes = [w.HANDLE, c.c_void_p]
k.CloseHandle.argtypes = [w.HANDLE]
k.Thread32First.argtypes = [w.HANDLE, c.c_void_p]
k.Thread32Next.argtypes = [w.HANDLE, c.c_void_p]
k.OpenProcess.restype = w.HANDLE
k.OpenThread.restype = w.HANDLE
k.CreateToolhelp32Snapshot.restype = w.HANDLE
k.GetThreadDescription.argtypes = [w.HANDLE, c.POINTER(c.c_wchar_p)]
dbghelp.SymInitialize.argtypes = [w.HANDLE, c.c_char_p, w.BOOL]
dbghelp.SymFunctionTableAccess64.restype = c.c_void_p
dbghelp.SymFunctionTableAccess64.argtypes = [w.HANDLE, c.c_uint64]
dbghelp.SymGetModuleBase64.restype = c.c_uint64
dbghelp.SymGetModuleBase64.argtypes = [w.HANDLE, c.c_uint64]
FTA = c.WINFUNCTYPE(c.c_void_p, w.HANDLE, c.c_uint64)
GMB = c.WINFUNCTYPE(c.c_uint64, w.HANDLE, c.c_uint64)
dbghelp.StackWalk64.argtypes = [w.DWORD, w.HANDLE, w.HANDLE, c.POINTER(STACKFRAME64), c.c_void_p,
                                c.c_void_p, FTA, GMB, c.c_void_p]

pid = int(sys.argv[1])
name = sys.argv[2]
samples = int(sys.argv[3]) if len(sys.argv) > 3 else 400
interval = (int(sys.argv[4]) if len(sys.argv) > 4 else 15) / 1000
out = Path(sys.argv[5]) if len(sys.argv) > 5 else Path("stacks.txt")

hp = k.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
if not hp or not dbghelp.SymInitialize(hp, None, True):
    sys.exit(f"cannot open process {pid} for stack walking ({c.get_last_error()})")
fta = FTA(dbghelp.SymFunctionTableAccess64)
gmb = GMB(dbghelp.SymGetModuleBase64)

arr = (w.HMODULE * 4096)()
needed = w.DWORD()
psapi.EnumProcessModulesEx(hp, arr, c.sizeof(arr), c.byref(needed), 3)
mods = []
for i in range(needed.value // c.sizeof(w.HMODULE)):
    fname = c.create_unicode_buffer(512)
    psapi.GetModuleFileNameExW(hp, arr[i], fname, 512)
    info = MODINFO()
    psapi.GetModuleInformation(hp, arr[i], c.byref(info), c.sizeof(info))
    mods.append((info.base or 0, info.size, Path(fname.value).name))


def where(addr):
    for base, size, mod in mods:
        if base <= addr < base + size:
            return f"{mod}+0x{addr - base:x}"
    return f"anon@0x{addr & ~0xfff:x}"


snap = k.CreateToolhelp32Snapshot(0x4, 0)
e = THREADENTRY32()
e.dwSize = c.sizeof(e)
thread = None
ok = k.Thread32First(snap, c.byref(e))
while ok:
    if e.th32OwnerProcessID == pid:
        h = k.OpenThread(THREAD_ACCESS, False, e.th32ThreadID)
        desc = c.c_wchar_p()
        if h and k.GetThreadDescription(h, c.byref(desc)) >= 0 and desc.value == name:
            thread = h
            break
        if h:
            k.CloseHandle(h)
    ok = k.Thread32Next(snap, c.byref(e))
if not thread:
    sys.exit(f"no thread named {name}")

buf = (c.c_byte * (c.sizeof(CONTEXT) + 16))()
ctx = CONTEXT.from_address((c.addressof(buf) + 15) & ~15)
stacks = collections.Counter()
for _ in range(samples):
    if k.SuspendThread(thread) == 0xFFFFFFFF:
        break
    try:
        ctx.ContextFlags = CONTEXT_FULL
        if not k.GetThreadContext(thread, c.byref(ctx)):
            continue
        frame = STACKFRAME64()
        frame.AddrPC.Offset, frame.AddrPC.Mode = ctx.Rip, 3
        frame.AddrFrame.Offset, frame.AddrFrame.Mode = ctx.Rbp, 3
        frame.AddrStack.Offset, frame.AddrStack.Mode = ctx.Rsp, 3
        chain = []
        for _depth in range(96):
            if not dbghelp.StackWalk64(IMAGE_FILE_MACHINE_AMD64, hp, thread, c.byref(frame),
                                       c.byref(ctx), None, fta, gmb, None):
                break
            if frame.AddrPC.Offset == 0:
                break
            chain.append(frame.AddrPC.Offset)
    finally:
        k.ResumeThread(thread)
    stacks[";".join(where(a) for a in chain)] += 1
    time.sleep(interval)
with open(out, "w") as f:
    for stack, count in stacks.most_common():
        f.write(f"{count} {stack}\n")
print(f"{sum(stacks.values())} samples, {len(stacks)} distinct stacks -> {out}")
