"""Identity of a test process this harness launched, and termination of that process only.

A process is identified by pid + creation time + executable path, recorded at launch. A pid alone
is not enough: once the test emulator exits, Windows can hand its pid to another process (the
user's own emulator session included). Nothing here ever looks processes up by image name.
"""
import ctypes as c
import ctypes.wintypes as w

PROCESS_TERMINATE = 0x0001
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
SYNCHRONIZE = 0x00100000
STILL_ACTIVE = 259

kernel32 = c.WinDLL("kernel32", use_last_error=True)
kernel32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel32.OpenProcess.restype = w.HANDLE
kernel32.GetProcessTimes.argtypes = [w.HANDLE] + [c.POINTER(w.FILETIME)] * 4
kernel32.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
kernel32.GetExitCodeProcess.argtypes = [w.HANDLE, c.POINTER(w.DWORD)]
kernel32.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
kernel32.CloseHandle.argtypes = [w.HANDLE]


def _identity_of(handle):
    created, exited, kernel, user = w.FILETIME(), w.FILETIME(), w.FILETIME(), w.FILETIME()
    if not kernel32.GetProcessTimes(handle, c.byref(created), c.byref(exited), c.byref(kernel),
                                    c.byref(user)):
        return None
    size = w.DWORD(32768)
    buf = c.create_unicode_buffer(size.value)
    if not kernel32.QueryFullProcessImageNameW(handle, 0, buf, c.byref(size)):
        return None
    return (created.dwHighDateTime << 32) | created.dwLowDateTime, buf.value.lower()


def identity(pid):
    """{'pid', 'created', 'exe'} of a live process, or None."""
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return None
    try:
        found = _identity_of(handle)
        return None if found is None else {"pid": pid, "created": found[0], "exe": found[1]}
    finally:
        kernel32.CloseHandle(handle)


def is_same(owned):
    """True while the recorded process (same pid, creation time and executable) is running."""
    current = identity(owned["pid"])
    if current is None or current["created"] != owned["created"] or current["exe"] != owned["exe"]:
        return False
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, owned["pid"])
    if not handle:
        return False
    try:
        code = w.DWORD()
        return bool(kernel32.GetExitCodeProcess(handle, c.byref(code))) and code.value == STILL_ACTIVE
    finally:
        kernel32.CloseHandle(handle)


def terminate(owned):
    """Terminates the recorded process if (and only if) it is still that process."""
    handle = kernel32.OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, False,
                                  owned["pid"])
    if not handle:
        return False
    try:
        found = _identity_of(handle)
        if found is None or found != (owned["created"], owned["exe"]):
            return False
        return bool(kernel32.TerminateProcess(handle, 1))
    finally:
        kernel32.CloseHandle(handle)
