"""Save a PNG of the emulator window for rendering-correctness checks.

Uses PrintWindow(PW_RENDERFULLCONTENT) so an occluded window is still captured
through DWM; falls back to a screen-DC BitBlt of the window rectangle.
Also returns simple image statistics (mean luma, fraction of near-black pixels)
so an all-black or frozen frame is detectable without viewing the image.

Usage: python capture_window.py PID OUT.png
"""
import ctypes as c
import ctypes.wintypes as w
import json
import re
import struct
import sys
import zlib

user32 = c.WinDLL("user32", use_last_error=True)
gdi32 = c.WinDLL("gdi32", use_last_error=True)
user32.SetProcessDPIAware()
EnumWindowsProc = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
user32.EnumWindows.argtypes = [EnumWindowsProc, w.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
user32.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user32.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
user32.ClientToScreen.argtypes = [w.HWND, c.POINTER(w.POINT)]
user32.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
user32.GetDC.argtypes = [w.HWND]
user32.GetDC.restype = w.HDC
user32.ReleaseDC.argtypes = [w.HWND, w.HDC]
gdi32.CreateCompatibleDC.argtypes = [w.HDC]
gdi32.CreateCompatibleDC.restype = w.HDC
gdi32.CreateCompatibleBitmap.argtypes = [w.HDC, c.c_int, c.c_int]
gdi32.CreateCompatibleBitmap.restype = w.HBITMAP
gdi32.SelectObject.argtypes = [w.HDC, w.HGDIOBJ]
gdi32.SelectObject.restype = w.HGDIOBJ
gdi32.BitBlt.argtypes = [w.HDC, c.c_int, c.c_int, c.c_int, c.c_int, w.HDC, c.c_int, c.c_int, w.DWORD]
gdi32.GetDIBits.argtypes = [w.HDC, w.HBITMAP, w.UINT, w.UINT, c.c_void_p, c.c_void_p, w.UINT]
gdi32.DeleteObject.argtypes = [w.HGDIOBJ]
gdi32.DeleteDC.argtypes = [w.HDC]


def find_window(pid):
    found = []

    def cb(hwnd, _):
        owner = w.DWORD()
        user32.GetWindowThreadProcessId(hwnd, c.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            buf = c.create_unicode_buffer(512)
            user32.GetWindowTextW(hwnd, buf, 512)
            if re.search(r"frame: \d+", buf.value):
                found.append((hwnd, buf.value))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return found[0] if found else (None, None)


def grab(hwnd, use_printwindow):
    rc = w.RECT()
    user32.GetClientRect(hwnd, c.byref(rc))
    width, height = rc.right, rc.bottom
    screen = user32.GetDC(None)
    mem = gdi32.CreateCompatibleDC(screen)
    bmp = gdi32.CreateCompatibleBitmap(screen, width, height)
    old = gdi32.SelectObject(mem, bmp)
    if use_printwindow:
        ok = user32.PrintWindow(hwnd, mem, 0x1 | 0x2)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    else:
        pt = w.POINT(0, 0)
        user32.ClientToScreen(hwnd, c.byref(pt))
        ok = gdi32.BitBlt(mem, 0, 0, width, height, screen, pt.x, pt.y, 0x00CC0020 | 0x40000000)
    header = struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = c.create_string_buffer(width * height * 4)
    gdi32.GetDIBits(mem, bmp, 0, height, pixels, header, 0)
    gdi32.SelectObject(mem, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(None, screen)
    return bool(ok), width, height, pixels.raw


def stats(width, height, bgra):
    step = 16
    total = dark = 0
    luma_sum = 0.0
    for y in range(0, height, step):
        row = y * width * 4
        for x in range(0, width, step):
            b, g, r = bgra[row + x * 4: row + x * 4 + 3]
            luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
            luma_sum += luma
            dark += luma < 8
            total += 1
    return dict(mean_luma=luma_sum / max(total, 1), near_black_fraction=dark / max(total, 1))


def write_png(path, width, height, bgra):
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        row = bgra[y * width * 4:(y + 1) * width * 4]
        rgb = bytearray(width * 3)
        rgb[0::3] = row[2::4]
        rgb[1::3] = row[1::4]
        rgb[2::3] = row[0::4]
        raw += rgb

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    pid, out = int(sys.argv[1]), sys.argv[2]
    hwnd, title = find_window(pid)
    if not hwnd:
        raise SystemExit(json.dumps(dict(error="window not found", pid=pid)))
    # PrintWindow can return a stale DWM redirection surface for a Vulkan flip-model
    # swapchain, so always save the on-screen pixels too (valid only if unoccluded;
    # "foreground" records whether it was). Compare both when judging a frame.
    # Screen pixels are only the game's when it is the foreground window; otherwise
    # they would be whatever covers it, so they are not saved.
    result = dict(title=title, foreground=user32.GetForegroundWindow() == hwnd)
    ok2, width2, height2, px2 = grab(hwnd, True)
    write_png(out, width2, height2, px2)
    result.update(printwindow=dict(stats(width2, height2, px2), ok=ok2, path=out), width=width2, height=height2)
    if result["foreground"]:
        ok, width, height, px = grab(hwnd, False)
        screen_out = out[:-4] + "-screen.png" if out.lower().endswith(".png") else out + "-screen.png"
        write_png(screen_out, width, height, px)
        result.update(screen=dict(stats(width, height, px), ok=ok, path=screen_out))
    print(json.dumps(result))
