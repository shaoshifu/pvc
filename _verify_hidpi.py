# -*- coding: utf-8 -*-
"""Capture the real fullscreen client area without touching the player's save."""
import ctypes
import os
import subprocess
import time

import numpy as np
from PIL import Image

D = os.path.dirname(os.path.abspath(__file__))
env = dict(os.environ)
env["PVZ_SAVE_FILE"] = os.path.join(D, "_hidpi_test_save.dat")
p = subprocess.Popen([os.path.join(D, "pvz.exe")], cwd=D, env=env)


class Rect(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class BitmapInfoHeader(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("width", ctypes.c_long),
                ("height", ctypes.c_long), ("planes", ctypes.c_uint16),
                ("bit_count", ctypes.c_uint16), ("compression", ctypes.c_uint32),
                ("image_size", ctypes.c_uint32), ("xppm", ctypes.c_long),
                ("yppm", ctypes.c_long), ("used", ctypes.c_uint32),
                ("important", ctypes.c_uint32)]


try:
    time.sleep(3)
    user = ctypes.windll.user32
    gdi = ctypes.windll.gdi32
    hwnd = user.FindWindowW("PvZClassC", None)
    if not hwnd:
        raise RuntimeError("game window not found")

    user.PostMessageW(hwnd, 0x0100, 0x7A, 0)  # F11
    user.PostMessageW(hwnd, 0x0101, 0x7A, 0)
    time.sleep(2)
    user.PostMessageW(hwnd, 0x0100, 0x52, 0)  # R -> play
    user.PostMessageW(hwnd, 0x0101, 0x52, 0)
    time.sleep(2)

    rc = Rect()
    user.GetClientRect(hwnd, ctypes.byref(rc))
    width, height = rc.right, rc.bottom
    window_dc = user.GetDC(hwnd)
    memory_dc = gdi.CreateCompatibleDC(window_dc)
    bitmap = gdi.CreateCompatibleBitmap(window_dc, width, height)
    old = gdi.SelectObject(memory_dc, bitmap)
    user.PrintWindow(hwnd, memory_dc, 3)
    info = BitmapInfoHeader(40, width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = ctypes.create_string_buffer(width * height * 4)
    gdi.GetDIBits(memory_dc, bitmap, 0, height, pixels, ctypes.byref(info), 0)
    gdi.SelectObject(memory_dc, old)
    gdi.DeleteObject(bitmap)
    gdi.DeleteDC(memory_dc)
    user.ReleaseDC(hwnd, window_dc)

    bgra = np.frombuffer(pixels, np.uint8).reshape(height, width, 4)
    rgb = bgra[:, :, :3][:, :, ::-1].copy()
    output = os.path.join(D, "_hidpi_fullscreen.png")
    Image.fromarray(rgb).save(output)
    print("[OK] captured %dx%d -> %s" % (width, height, output))
finally:
    p.terminate()
    try:
        p.wait(timeout=3)
    except subprocess.TimeoutExpired:
        p.kill()
