"""往某个窗口里"打字"并回车 —— 用来做界面级端到端验收（不依赖任何 UI 自动化框架）。

用法：
  python misc/tools/ui-send.py --pid 37848 --click 1400,1200 --text "你好" --enter
  python misc/tools/ui-send.py --pid 37848 --click 1400,1200 --enter        # 只回车

坐标是**物理像素**（进程已做 DPI 感知），用 window-shot.py 抓图后按图上的比例换算。
发文字走剪贴板 + Ctrl+V，避免逐键 send 中文时被输入法吃掉。
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import sys
import time

u32 = ctypes.WinDLL('user32', use_last_error=True)

VK_CONTROL = 0x11
VK_V = 0x56
VK_RETURN = 0x0D
KEYEVENTF_KEYUP = 0x0002
INPUT_MOUSE = 0
INPUT_KEYBOARD = 1
VK = {'CTRL': 0x11, 'SHIFT': 0x10, 'ALT': 0x12, 'HOME': 0x24, 'END': 0x23,
      'RETURN': 0x0D, 'ENTER': 0x0D, 'ESC': 0x1B, 'V': 0x56, 'A': 0x41,
      'PRIOR': 0x21, 'NEXT': 0x22}
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_WHEEL = 0x0800


def make_dpi_aware():
    try:
        u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        return 'per-monitor-v2'
    except Exception:
        try:
            ctypes.WinDLL('shcore').SetProcessDpiAwareness(2)
            return 'per-monitor'
        except Exception:
            return 'none'


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [('dx', ctypes.c_long), ('dy', ctypes.c_long), ('mouseData', wt.DWORD),
                ('dwFlags', wt.DWORD), ('time', wt.DWORD),
                ('dwExtraInfo', ctypes.c_void_p)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [('wVk', wt.WORD), ('wScan', wt.WORD), ('dwFlags', wt.DWORD),
                ('time', wt.DWORD), ('dwExtraInfo', ctypes.c_void_p)]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [('uMsg', wt.DWORD), ('wParamL', wt.WORD), ('wParamH', wt.WORD)]


class _INPUTUNION(ctypes.Union):
    _fields_ = [('mi', MOUSEINPUT), ('ki', KEYBDINPUT), ('hi', HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ('u',)
    _fields_ = [('type', wt.DWORD), ('u', _INPUTUNION)]


u32.SendInput.restype = wt.UINT
u32.SendInput.argtypes = [wt.UINT, ctypes.POINTER(INPUT), ctypes.c_int]


def send(*inputs):
    """SendInput 的 cbSize 必须是完整联合体大小，给 24 字节会被直接拒收（返回 0）。"""
    arr = (INPUT * len(inputs))(*inputs)
    n = u32.SendInput(len(inputs), arr, ctypes.sizeof(INPUT))
    if n != len(inputs):
        raise RuntimeError(f'SendInput 只送出了 {n}/{len(inputs)} 个事件（err={ctypes.get_last_error()}）')
    return n


def key(vk, up=False):
    inp = INPUT(type=INPUT_KEYBOARD)
    inp.ki = KEYBDINPUT(wVk=vk, wScan=0, dwFlags=KEYEVENTF_KEYUP if up else 0, time=0, dwExtraInfo=None)
    send(inp)


def click(x, y):
    u32.SetCursorPos(int(x), int(y))
    time.sleep(0.15)
    down = INPUT(type=INPUT_MOUSE)
    down.mi = MOUSEINPUT(dx=0, dy=0, mouseData=0, dwFlags=MOUSEEVENTF_LEFTDOWN, time=0, dwExtraInfo=None)
    up = INPUT(type=INPUT_MOUSE)
    up.mi = MOUSEINPUT(dx=0, dy=0, mouseData=0, dwFlags=MOUSEEVENTF_LEFTUP, time=0, dwExtraInfo=None)
    send(down, up)


def wheel(pos, notches):
    """notches>0 向下滚（每格 -120），<0 向上。"""
    u32.SetCursorPos(int(pos[0]), int(pos[1]))
    time.sleep(0.15)
    for _ in range(abs(notches)):
        inp = INPUT(type=INPUT_MOUSE)
        inp.mi = MOUSEINPUT(dx=0, dy=0, mouseData=(-120 if notches > 0 else 120) & 0xFFFFFFFF,
                            dwFlags=MOUSEEVENTF_WHEEL, time=0, dwExtraInfo=None)
        send(inp)
        time.sleep(0.03)


def press(combo):
    """--key CTRL+HOME 这种组合键。"""
    parts = [p.strip().upper() for p in combo.split('+') if p.strip()]
    if not parts:
        return
    mods = [VK[p] for p in parts[:-1]]
    main = VK[parts[-1]]
    for m in mods:
        key(m)
    key(main)
    key(main, up=True)
    for m in reversed(mods):
        key(m, up=True)


def set_clipboard(text):
    CF_UNICODETEXT = 13
    GMEM_MOVEABLE = 0x0002
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    # 必须显式声明：默认 restype=c_int 会把 64 位句柄截断，GlobalLock 拿到 0 直接写崩
    k32.GlobalAlloc.restype = ctypes.c_void_p
    k32.GlobalAlloc.argtypes = [wt.UINT, ctypes.c_size_t]
    k32.GlobalLock.restype = ctypes.c_void_p
    k32.GlobalLock.argtypes = [ctypes.c_void_p]
    k32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    u32.SetClipboardData.restype = ctypes.c_void_p
    u32.SetClipboardData.argtypes = [wt.UINT, ctypes.c_void_p]
    u32.OpenClipboard.argtypes = [ctypes.c_void_p]

    u32.OpenClipboard(None)
    u32.EmptyClipboard()
    data = text.encode('utf-16-le') + b'\x00\x00'
    h = k32.GlobalAlloc(GMEM_MOVEABLE, len(data))
    p = k32.GlobalLock(h)
    if not p:
        u32.CloseClipboard()
        raise RuntimeError('GlobalLock 失败')
    ctypes.memmove(p, data, len(data))
    k32.GlobalUnlock(h)
    u32.SetClipboardData(CF_UNICODETEXT, h)
    u32.CloseClipboard()


def find_window(pid, title):
    found = []

    def cb(hwnd, _):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if not u32.IsWindowVisible(hwnd):
            return True
        n = u32.GetWindowTextLengthW(hwnd)
        if n <= 0:
            return True
        b = ctypes.create_unicode_buffer(n + 1)
        u32.GetWindowTextW(hwnd, b, n + 1)
        if pid and p.value != pid:
            return True
        if title and title.lower() not in b.value.lower():
            return True
        r = wt.RECT()
        u32.GetWindowRect(hwnd, ctypes.byref(r))
        found.append((hwnd, p.value, b.value, r.right - r.left, r.bottom - r.top))
        return True

    u32.EnumWindows(ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(cb), 0)
    if not found:
        return None
    return max(found, key=lambda f: f[3] * f[4])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int)
    ap.add_argument('--title')
    ap.add_argument('--click', help='物理坐标 X,Y')
    ap.add_argument('--text')
    ap.add_argument('--enter', action='store_true')
    ap.add_argument('--wheel', type=int, default=0, help='滚轮格数（正=向下）')
    ap.add_argument('--wheel-at', default='1500,800', help='滚轮位置 X,Y')
    ap.add_argument('--key', action='append', default=[], help='组合键，如 CTRL+HOME；可重复')
    args = ap.parse_args()

    print(f'DPI 感知级别: {make_dpi_aware()}', file=sys.stderr)
    target = find_window(args.pid, args.title)
    if not target:
        print('没找到目标窗口', file=sys.stderr)
        return 2
    hwnd, pid, title = target[0], target[1], target[2]

    u32.ShowWindow(hwnd, 9)
    u32.SetForegroundWindow(hwnd)
    time.sleep(0.5)
    if u32.GetForegroundWindow() != hwnd:
        print('警告：目标窗口没拿到前台焦点，为免误输入已中止', file=sys.stderr)
        return 3
    print(f'目标: {title!r} pid={pid}')

    if args.click:
        x, y = (int(v) for v in args.click.split(','))
        click(x, y)
        time.sleep(0.35)
        print(f'已在 ({x},{y}) 点击')

    for combo in args.key:
        press(combo)
        time.sleep(0.4)
        print(f'已按键 {combo}')

    if args.text:
        set_clipboard(args.text)
        time.sleep(0.15)
        key(VK_CONTROL)
        key(VK_V)
        key(VK_V, up=True)
        key(VK_CONTROL, up=True)
        time.sleep(0.4)
        print(f'已粘贴 {len(args.text)} 字')

    if args.wheel:
        pos = tuple(int(v) for v in args.wheel_at.split(','))
        wheel(pos, args.wheel)
        time.sleep(0.5)
        print(f'已滚动 {args.wheel} 格 @ {pos}')

    if args.enter:
        key(VK_RETURN)
        key(VK_RETURN, up=True)
        print('已回车')
    return 0


if __name__ == '__main__':
    sys.exit(main())
