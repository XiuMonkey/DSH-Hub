"""把某个进程的顶层窗口抓成 PNG —— 用于"界面到底显示了什么"的取证。

用法：
  python misc/tools/window-shot.py --pid 37848 -o shot.png
  python misc/tools/window-shot.py --title "DSH Hub" -o shot.png
  python misc/tools/window-shot.py --list            # 只列出候选窗口

只依赖 ctypes + 标准库（PNG 由 zlib 手写），不装任何第三方包。
抓图用 PrintWindow(PW_RENDERFULLCONTENT)，窗口被遮挡或不在前台也能拿到内容。
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import zlib

u32 = ctypes.WinDLL('user32', use_last_error=True)
g32 = ctypes.WinDLL('gdi32', use_last_error=True)


def make_dpi_aware():
    """必须先做：否则在高 DPI 下 GetWindowRect 返回虚拟化尺寸，PrintWindow 只拿到左上角一小块。"""
    try:
        u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))   # PER_MONITOR_AWARE_V2
        return 'per-monitor-v2'
    except Exception:
        pass
    try:
        ctypes.WinDLL('shcore').SetProcessDpiAwareness(2)
        return 'per-monitor'
    except Exception:
        pass
    try:
        u32.SetProcessDPIAware()
        return 'system'
    except Exception:
        return 'none'

PW_RENDERFULLCONTENT = 0x00000002
BI_RGB = 0
DIB_RGB_COLORS = 0
SRCCOPY = 0x00CC0020


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [('biSize', wt.DWORD), ('biWidth', ctypes.c_long), ('biHeight', ctypes.c_long),
                ('biPlanes', wt.WORD), ('biBitCount', wt.WORD), ('biCompression', wt.DWORD),
                ('biSizeImage', wt.DWORD), ('biXPelsPerMeter', ctypes.c_long),
                ('biYPelsPerMeter', ctypes.c_long), ('biClrUsed', wt.DWORD),
                ('biClrImportant', wt.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [('bmiHeader', BITMAPINFOHEADER), ('bmiColors', wt.DWORD * 3)]


def visible_windows():
    """返回 [(hwnd, pid, 标题, 宽, 高)]，只要可见且有标题的顶层窗口。"""
    found = []

    def cb(hwnd, _):
        pid = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if not u32.IsWindowVisible(hwnd):
            return True
        n = u32.GetWindowTextLengthW(hwnd)
        if n <= 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        u32.GetWindowTextW(hwnd, buf, n + 1)
        rect = wt.RECT()
        u32.GetWindowRect(hwnd, ctypes.byref(rect))
        found.append((hwnd, pid.value, buf.value, rect.right - rect.left, rect.bottom - rect.top))
        return True

    WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    u32.EnumWindows(WNDENUMPROC(cb), 0)
    return found


def write_png(path, width, height, bgra):
    """bgra: 自下而上的 BGRA 字节串（DIB 原生顺序）-> 写成 RGBA PNG。"""
    rows = []
    stride = width * 4
    for y in range(height - 1, -1, -1):          # DIB 底行在前，PNG 顶行在前
        row = bgra[y * stride:(y + 1) * stride]
        rows.append(b'\x00' + _bgra_to_rgba(row))   # 每行前置 filter 字节 0
    raw = b''.join(rows)

    def chunk(tag, data):
        body = tag + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as fh:
        fh.write(png)


def _bgra_to_rgba(row):
    out = bytearray(len(row))
    for i in range(0, len(row), 4):
        out[i] = row[i + 2]
        out[i + 1] = row[i + 1]
        out[i + 2] = row[i]
        out[i + 3] = 255
    return bytes(out)


def row_stats(hwnd, x_from_ratio=0.55):
    """逐行统计非背景像素（右侧区域），用来在没有第三方图像库时定位界面的横向分带。"""
    rect = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(rect))
    width, height = rect.right - rect.left, rect.bottom - rect.top
    hdc_win = u32.GetWindowDC(hwnd)
    hdc_mem = g32.CreateCompatibleDC(hdc_win)
    hbmp = g32.CreateCompatibleBitmap(hdc_win, width, height)
    g32.SelectObject(hdc_mem, hbmp)
    if not u32.PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT):
        u32.PrintWindow(hwnd, hdc_mem, 0)
    info = BITMAPINFO()
    info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    info.bmiHeader.biWidth = width
    info.bmiHeader.biHeight = height
    info.bmiHeader.biPlanes = 1
    info.bmiHeader.biBitCount = 32
    info.bmiHeader.biCompression = BI_RGB
    buf = ctypes.create_string_buffer(width * height * 4)
    g32.GetDIBits(hdc_mem, hbmp, 0, height, buf, ctypes.byref(info), DIB_RGB_COLORS)
    g32.DeleteObject(hbmp)
    g32.DeleteDC(hdc_mem)
    u32.ReleaseDC(hwnd, hdc_win)
    raw = buf.raw
    x0 = int(width * x_from_ratio)
    print(f'窗口 {width}x{height}，统计 x>={x0} 的非白像素数（行号自上而下）')
    for y in range(height):
        base = (height - 1 - y) * width * 4
        n = 0
        for x in range(x0, width, 2):
            i = base + x * 4
            if raw[i] < 235 or raw[i + 1] < 235 or raw[i + 2] < 235:
                n += 1
        print(f'{y:>4} {"#" * min(n // 3, 120)}')


def capture_screen(path, rect):
    """从桌面 DC 抓屏幕真值 —— Qt 无边框窗口的 PrintWindow 结果可能不可信。"""
    x, y, width, height = rect
    hdc_screen = u32.GetDC(0)
    hdc_mem = g32.CreateCompatibleDC(hdc_screen)
    hbmp = g32.CreateCompatibleBitmap(hdc_screen, width, height)
    g32.SelectObject(hdc_mem, hbmp)
    g32.BitBlt(hdc_mem, 0, 0, width, height, hdc_screen, x, y, SRCCOPY)

    info = BITMAPINFO()
    info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    info.bmiHeader.biWidth = width
    info.bmiHeader.biHeight = height
    info.bmiHeader.biPlanes = 1
    info.bmiHeader.biBitCount = 32
    info.bmiHeader.biCompression = BI_RGB
    buf = ctypes.create_string_buffer(width * height * 4)
    g32.GetDIBits(hdc_mem, hbmp, 0, height, buf, ctypes.byref(info), DIB_RGB_COLORS)
    g32.DeleteObject(hbmp)
    g32.DeleteDC(hdc_mem)
    u32.ReleaseDC(0, hdc_screen)
    write_png(path, width, height, buf.raw)
    return width, height


def capture(hwnd, path, crop_bottom=0):
    rect = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(rect))
    width, height = rect.right - rect.left, rect.bottom - rect.top
    if width <= 0 or height <= 0:
        raise RuntimeError(f'窗口尺寸异常: {width}x{height}')

    hdc_win = u32.GetWindowDC(hwnd)
    hdc_mem = g32.CreateCompatibleDC(hdc_win)
    hbmp = g32.CreateCompatibleBitmap(hdc_win, width, height)
    g32.SelectObject(hdc_mem, hbmp)

    ok = u32.PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT)
    if not ok:
        ok = u32.PrintWindow(hwnd, hdc_mem, 0)
    if not ok:
        g32.BitBlt(hdc_mem, 0, 0, width, height, hdc_win, 0, 0, SRCCOPY)

    info = BITMAPINFO()
    info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    info.bmiHeader.biWidth = width
    info.bmiHeader.biHeight = height          # 正数 = 自下而上
    info.bmiHeader.biPlanes = 1
    info.bmiHeader.biBitCount = 32
    info.bmiHeader.biCompression = BI_RGB

    buf = ctypes.create_string_buffer(width * height * 4)
    got = g32.GetDIBits(hdc_mem, hbmp, 0, height, buf, ctypes.byref(info), DIB_RGB_COLORS)

    g32.DeleteObject(hbmp)
    g32.DeleteDC(hdc_mem)
    u32.ReleaseDC(hwnd, hdc_win)

    if got == 0:
        raise RuntimeError('GetDIBits 失败')
    raw = buf.raw
    out_h = height
    if crop_bottom > 0 and crop_bottom < height:
        # DIB 是自下而上的：底部就是前 crop_bottom 行
        out_h = crop_bottom
        raw = raw[:width * out_h * 4]
    write_png(path, width, out_h, raw)
    return width, out_h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int)
    ap.add_argument('--title')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('-o', '--out', default='window.png')
    ap.add_argument('--crop-bottom', type=int, default=0, help='只保留窗口底部 N 像素（便于看输入区）')
    ap.add_argument('--rowstats', action='store_true', help='打印逐行像素统计（定位界面分带）')
    ap.add_argument('--screen', action='store_true', help='改抓屏幕真值（Qt 无边框窗口 PrintWindow 不可信时用）')
    ap.add_argument('--focus', action='store_true', help='抓之前把目标窗口置前（配合 --screen）')
    ap.add_argument('--resize', help='先改窗口尺寸 WxH（物理像素；可超过屏幕，PrintWindow 仍能整窗渲染）')
    args = ap.parse_args()

    level = make_dpi_aware()
    print(f'DPI 感知级别: {level}', file=sys.stderr)
    wins = visible_windows()
    if args.list:
        for hwnd, pid, title, w, h in wins:
            if args.title and args.title.lower() not in title.lower():
                continue
            if args.pid and pid != args.pid:
                continue
            print(f'{hwnd:#010x}  pid={pid:<8} {w}x{h}  {title}')
        return 0

    target = None
    for hwnd, pid, title, w, h in wins:
        if args.pid and pid != args.pid:
            continue
        if args.title and args.title.lower() not in title.lower():
            continue
        if target is None or w * h > target[3] * target[4]:
            target = (hwnd, pid, title, w, h)
    if target is None:
        print('没找到匹配的可见窗口', file=sys.stderr)
        return 2

    hwnd, pid, title, _, _ = target
    if args.resize:
        w, h = (int(v) for v in args.resize.lower().split('x'))
        SWP_NOZORDER = 0x0004
        u32.SetWindowPos(hwnd, 0, 0, 0, w, h, SWP_NOZORDER)
        import time
        time.sleep(0.6)
        print(f'窗口已改为 {w}x{h}')
    if args.rowstats:
        row_stats(hwnd)
        return 0
    if args.screen:
        import time
        if args.focus:
            u32.ShowWindow(hwnd, 9)          # SW_RESTORE
            u32.SetForegroundWindow(hwnd)
            time.sleep(0.6)
        rect = wt.RECT()
        u32.GetWindowRect(hwnd, ctypes.byref(rect))
        w, h = capture_screen(args.out, (rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top))
        print(f'屏幕真值 {w}x{h}  @({rect.left},{rect.top})  -> {args.out}')
        return 0
    w, h = capture(hwnd, args.out, args.crop_bottom)
    print(f'已抓取 {w}x{h}  pid={pid}  title={title!r}  -> {args.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
