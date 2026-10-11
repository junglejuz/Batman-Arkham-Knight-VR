"""Arkham Knight menu helper for tests (JJ 2026-10-07: I may reach gameplay myself - click the title screen once, click
the current save, then "Continue Story"). Every click is screenshot-verified by the caller first.
Usage:
  python akmenu.py shot <file.png>          screenshot of the game's client area (prints its size)
  python akmenu.py click <fx> <fy>          real left click at a FRACTION of the client area (0..1, 0..1)
  python akmenu.py key <vk>                 one key press, e.g. 0x0D = Enter (game must be in front)
"""
import ctypes, ctypes.wintypes as w, sys, time

u = ctypes.windll.user32
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass


def game_pids():
    import subprocess
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq BatmanAK.exe', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    return {int(l.split('","')[1]) for l in out.splitlines() if l.startswith('"BatmanAK')}


def game_window():
    pids = game_pids()
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, w.HWND, w.LPARAM)
    def cb(h, l):
        n = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(h, n, 256)
        pid = w.DWORD(); u.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if 'Arkham Knight' in n.value and u.IsWindowVisible(h) and pid.value in pids:
            found.append(h)
        return True
    u.EnumWindows(cb, 0)
    return found[0] if found else None


def client_rect(h):
    r = w.RECT(); u.GetClientRect(h, ctypes.byref(r))
    p = w.POINT(0, 0); u.ClientToScreen(h, ctypes.byref(p))
    return p.x, p.y, r.right, r.bottom


def shot(path):
    from PIL import ImageGrab
    h = game_window()
    if not h:
        print('no game window'); return 1
    x, y, cw, ch = client_rect(h)
    img = ImageGrab.grab(bbox=(x, y, x + cw, y + ch), all_screens=True)
    img.save(path)
    print(f'{cw}x{ch} at {x},{y} -> {path}')
    return 0


def click(fx, fy):
    h = game_window()
    if not h:
        print('no game window'); return 1
    # FRONTFIRST 2026-10-08: a click lands on whatever window is on top - VS Code was in front and got the title click
    # (the game then sat one screen behind). Bring the game forward without clicking (no Alt), and refuse if it stays behind.
    if u.GetForegroundWindow() != h:
        fg = u.GetForegroundWindow(); cur = ctypes.windll.kernel32.GetCurrentThreadId(); fgt = u.GetWindowThreadProcessId(fg, None)
        u.AttachThreadInput(cur, fgt, True); u.BringWindowToTop(h); u.SetForegroundWindow(h); u.AttachThreadInput(cur, fgt, False)
        time.sleep(0.5)
    if u.GetForegroundWindow() != h:
        n = ctypes.create_unicode_buffer(256); u.GetWindowTextW(u.GetForegroundWindow(), n, 256)
        print(f'game is not in front - click not sent (in front: "{n.value}")'); return 1
    x, y, cw, ch = client_rect(h)
    px, py = int(x + cw * fx), int(y + ch * fy)
    # OVERCHECK 2026-10-09: an always-on-top window of JJ's covered the game (the game was "in front" but the click point
    # was his window) - never click unless the window under the point IS the game
    class PT(ctypes.Structure): _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]
    u.WindowFromPoint.argtypes = [PT]; u.WindowFromPoint.restype = ctypes.c_void_p
    u.GetAncestor.argtypes = [ctypes.c_void_p, ctypes.c_uint]; u.GetAncestor.restype = ctypes.c_void_p
    under = u.GetAncestor(u.WindowFromPoint(PT(px, py)), 2)   # GA_ROOT
    if under != h:
        n = ctypes.create_unicode_buffer(256); u.GetWindowTextW(ctypes.c_void_p(under), n, 256)
        print(f'another window covers the game at the click point ("{n.value}") - click not sent'); return 1
    # hover first: Scaleform menus need to see the mouse over an item before a press selects it
    u.SetCursorPos(px - 6, py - 6); time.sleep(0.15)
    u.SetCursorPos(px, py); time.sleep(0.5)
    u.mouse_event(0x0002, 0, 0, 0, 0)   # left down
    time.sleep(0.08)
    u.mouse_event(0x0004, 0, 0, 0, 0)   # left up
    print(f'clicked {fx:.3f},{fy:.3f} = screen {px},{py}')
    return 0


def key(vk):
    """one key press (the game must be the window in front: a click on it first)"""
    h = game_window()
    if h and u.GetForegroundWindow() != h:
        # NOFOCUSCLICK 2026-10-08: a focus click INSIDE the picture (0.75/0.45) landed on a menu item at a smaller window
        # size and a New Game was started (JJ had to close it). Bring the game forward WITHOUT clicking first
        # (attach to the foreground thread, no Alt).
        fg = u.GetForegroundWindow()
        cur = ctypes.windll.kernel32.GetCurrentThreadId()
        fgt = u.GetWindowThreadProcessId(fg, None)
        u.AttachThreadInput(cur, fgt, True); u.BringWindowToTop(h); u.SetForegroundWindow(h); u.AttachThreadInput(cur, fgt, False)
        time.sleep(0.4)
    if h and u.GetForegroundWindow() != h:
        # last resort: a click on the window's TITLE BAR (outside the picture - never on a menu item)
        r = w.RECT(); u.GetWindowRect(h, ctypes.byref(r))
        x, y = (r.left + r.right) // 2, r.top + 12
        u.SetCursorPos(x, y); time.sleep(0.05); u.mouse_event(2, 0, 0, 0, 0); time.sleep(0.05); u.mouse_event(4, 0, 0, 0, 0)
        print(f'focus click on the title bar at {x},{y}'); time.sleep(0.4)
    if not h or u.GetForegroundWindow() != h:
        n = ctypes.create_unicode_buffer(256); u.GetWindowTextW(u.GetForegroundWindow(), n, 256)
        print(f'game is not in front - key not sent (in front: "{n.value}")'); return 1
    # KEYSCAN 2026-10-08 (HM run: M never opened the map, menus took Enter/Esc): the game's play keys read the SCAN code
    sc = u.MapVirtualKeyW(vk, 0)
    u.keybd_event(vk, sc, 0, 0); time.sleep(0.08); u.keybd_event(vk, sc, 2, 0)
    print(f'key {vk:#x} sent'); return 0


def hold(vk, secs):
    """hold a key (e.g. 0x53 = S, walk back) - game must be in front; same focus rules as key()"""
    h = game_window()
    if h and u.GetForegroundWindow() != h:
        fg = u.GetForegroundWindow(); cur = ctypes.windll.kernel32.GetCurrentThreadId(); fgt = u.GetWindowThreadProcessId(fg, None)
        u.AttachThreadInput(cur, fgt, True); u.BringWindowToTop(h); u.SetForegroundWindow(h); u.AttachThreadInput(cur, fgt, False)
        time.sleep(0.4)
    if not h or u.GetForegroundWindow() != h:
        print('game is not in front - key not held'); return 1
    sc = u.MapVirtualKeyW(vk, 0)
    u.keybd_event(vk, sc, 0, 0); time.sleep(secs); u.keybd_event(vk, sc, 2, 0)
    print(f'key {vk:#x} held {secs:.1f} s'); return 0


def mouse(dx, dy, steps=10):
    """relative mouse move (turns the game camera), split in steps - game must be in front"""
    h = game_window()
    if h and u.GetForegroundWindow() != h:
        fg = u.GetForegroundWindow(); cur = ctypes.windll.kernel32.GetCurrentThreadId(); fgt = u.GetWindowThreadProcessId(fg, None)
        u.AttachThreadInput(cur, fgt, True); u.BringWindowToTop(h); u.SetForegroundWindow(h); u.AttachThreadInput(cur, fgt, False)
        time.sleep(0.4)
    if not h or u.GetForegroundWindow() != h:
        print('game is not in front - mouse not moved'); return 1
    for _ in range(steps):
        u.mouse_event(0x0001, int(dx / steps), int(dy / steps), 0, 0); time.sleep(0.02)
    print(f'mouse moved {dx},{dy}'); return 0


if __name__ == '__main__':
    if len(sys.argv) >= 4 and sys.argv[1] == 'mouse':
        sys.exit(mouse(int(sys.argv[2]), int(sys.argv[3])))
    if len(sys.argv) >= 4 and sys.argv[1] == 'hold':
        sys.exit(hold(int(sys.argv[2], 0), float(sys.argv[3])))
    if len(sys.argv) >= 3 and sys.argv[1] == 'key':
        sys.exit(key(int(sys.argv[2], 0)))
    if len(sys.argv) >= 3 and sys.argv[1] == 'shot':
        sys.exit(shot(sys.argv[2]))
    if len(sys.argv) >= 4 and sys.argv[1] == 'click':
        sys.exit(click(float(sys.argv[2]), float(sys.argv[3])))
    print(__doc__)
