import ctypes, ctypes.wintypes as w, sys
from PIL import ImageGrab
u=ctypes.windll.user32
ctypes.windll.shcore.SetProcessDpiAwareness(2)
found=[]
@ctypes.WINFUNCTYPE(ctypes.c_bool, w.HWND, w.LPARAM)
def cb(h,l):
    n=ctypes.create_unicode_buffer(256); u.GetWindowTextW(h,n,256)
    if 'Arkham Knight' in n.value and u.IsWindowVisible(h): found.append(h)
    return True
u.EnumWindows(cb,0)
h=found[0]; r=w.RECT(); u.GetClientRect(h,ctypes.byref(r)); p=w.POINT(0,0); u.ClientToScreen(h,ctypes.byref(p))
print('client', r.right, r.bottom, 'at', p.x, p.y)
img=ImageGrab.grab(bbox=(p.x,p.y,p.x+r.right,p.y+r.bottom), all_screens=True)
img.save(sys.argv[1]); print(img.size)
