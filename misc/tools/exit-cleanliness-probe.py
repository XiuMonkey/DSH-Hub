"""退出路径验收：启动一个 DSH Hub，模拟点标题栏关闭按钮，看进程是否干净退完。

用法（从仓库根目录跑）：
    python misc/tools/exit-cleanliness-probe.py "<exe 路径>" [--settle-ms 12000] [--exit-timeout-ms 25000] [--label 名称]

为什么需要它：关窗后残留的"僵尸进程"（窗口没了、线程掉到 1 个、DLL 还被锁着、taskkill 也杀不掉）
只有真机跑一遍关窗路径才复现得出来。本脚本把这一遍固定成可重复的对照实验：同一个扩展、
同一个脚本，只换宿主 exe，就能直接看出退出路径上改动的效果。

判据（关键一条）：进程存在但 GetExitCodeProcess 返回值**不是 259**（STILL_ACTIVE），
说明它已经进入退出流程却没退完 —— 那就是僵尸，剩下的线程数与模块数只是佐证。
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
u32 = ctypes.WinDLL('user32', use_last_error=True)

WM_CLOSE = 0x0010
STILL_ACTIVE = 259
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
SYNCHRONIZE = 0x00100000
TH32CS_SNAPPROCESS = 0x00000002
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)), ("th32ModuleID", wt.DWORD),
                ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD),
                ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)), ("modBaseSize", wt.DWORD),
                ("hModule", wt.HMODULE), ("szModule", ctypes.c_wchar * 256),
                ("szExePath", ctypes.c_wchar * 260)]


def _snapshot(kind):
    snap = k32.CreateToolhelp32Snapshot(kind, 0)
    return snap if snap != ctypes.c_void_p(-1).value else None


def processes():
    """返回 [(pid, ppid, exeName, threadCount)]"""
    snap = _snapshot(TH32CS_SNAPPROCESS)
    if not snap:
        return []
    out = []
    pe = PROCESSENTRY32W()
    pe.dwSize = ctypes.sizeof(pe)
    ok = k32.Process32FirstW(snap, ctypes.byref(pe))
    while ok:
        out.append((pe.th32ProcessID, pe.th32ParentProcessID, pe.szExeFile, pe.cntThreads))
        ok = k32.Process32NextW(snap, ctypes.byref(pe))
    k32.CloseHandle(snap)
    return out


def modules_of(pid):
    """返回该进程已映射的模块名列表（不需要打开进程，跨会话也能读）"""
    snap = _snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32)
    if not snap:
        return []
    # 快照是整机的，按 pid 过滤；逐个 pid 取更省事：这里用 Module32First 循环配 th32ProcessID 字段不可靠，
    # 故改用每次为 pid 单独开快照
    k32.CloseHandle(snap)
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap == ctypes.c_void_p(-1).value:
        return []
    names = []
    me = MODULEENTRY32W()
    me.dwSize = ctypes.sizeof(me)
    ok = k32.Module32FirstW(snap, ctypes.byref(me))
    while ok:
        names.append(me.szModule)
        ok = k32.Module32NextW(snap, ctypes.byref(me))
    k32.CloseHandle(snap)
    return names


def open_for_query(pid):
    return k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, False, pid)


def exit_code(pid):
    h = open_for_query(pid)
    if not h:
        return None
    code = wt.DWORD(0)
    ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
    k32.CloseHandle(h)
    return code.value if ok else None


def is_alive(pid):
    h = open_for_query(pid)
    if not h:
        return False
    code = wt.DWORD(0)
    k32.GetExitCodeProcess(h, ctypes.byref(code))
    k32.CloseHandle(h)
    return code.value == STILL_ACTIVE


def visible_windows(pid):
    found = []

    def cb(hwnd, _):
        wpid = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == pid and u32.IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(512)
            u32.GetWindowTextW(hwnd, buf, 512)
            found.append((hwnd, buf.value))
        return True

    u32.EnumWindows(ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(cb), 0)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe', help='DSH Hub.exe 的绝对路径')
    ap.add_argument('--settle-ms', type=int, default=12000, help='窗口出现后再等多久（默认 12000，让扩展 attach 并起后端）')
    ap.add_argument('--window-timeout-ms', type=int, default=40000, help='等主窗口出现的最长时间')
    ap.add_argument('--exit-timeout-ms', type=int, default=25000, help='发出 WM_CLOSE 后等进程退出的最长时间')
    ap.add_argument('--label', default='', help='本次实验的名字（打印用）')
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.isfile(exe):
        print(f'找不到 exe：{exe}')
        return 2
    label = args.label or os.path.basename(os.path.dirname(exe))

    env = dict(os.environ)
    # exe 旁边没有 Qt dll，跑起来要靠 PATH 上的 Qt bin（与 rebuild.sh 的约定一致）
    qt_bin = r'D:\Qt\6.11.2\msvc2022_64\bin'
    if os.path.isdir(qt_bin) and qt_bin.lower() not in env.get('PATH', '').lower():
        env['PATH'] = qt_bin + os.pathsep + env.get('PATH', '')

    print(f'== {label} ==')
    print(f'   exe: {exe}')
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env)
    pid = proc.pid
    print(f'   启动 pid={pid}')

    # 1) 等主窗口
    deadline = time.time() + args.window_timeout_ms / 1000.0
    hwnd = None
    while time.time() < deadline:
        wins = [w for w in visible_windows(pid) if w[1].strip()]
        if wins:
            hwnd, title = wins[0]
            print(f'   主窗口出现：hwnd=0x{hwnd:X} 标题="{title}"（另有 {len(wins) - 1} 个可见窗口）')
            break
        if not is_alive(pid):
            print('   进程在窗口出现前就退出了')
            return 2
        time.sleep(0.4)
    if not hwnd:
        print('   等窗口超时，放弃')
        return 2

    # 2) 让它把扩展挂上、后端起起来
    time.sleep(args.settle_ms / 1000.0)
    kids = [p for p in processes() if p[1] == pid]
    print(f'   稳定期结束：本进程线程={next((x[3] for x in processes() if x[0] == pid), "?")}，'
          f'子进程={[f"{k[2]}({k[0]})" for k in kids] or "无"}')
    mods_before = modules_of(pid)
    print(f'   已映射模块={len(mods_before)}，其中插件 dll：'
          f'{[m for m in mods_before if m.lower().endswith(".dll") and "main" in m.lower()] or "无"}')

    # 3) 模拟点关闭按钮
    print('   发送 WM_CLOSE ...')
    u32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    t0 = time.time()

    # 4) 等它退完
    gone_after = None
    while time.time() - t0 < args.exit_timeout_ms / 1000.0:
        if not is_alive(pid):
            gone_after = time.time() - t0
            break
        time.sleep(0.25)

    print('')
    if gone_after is not None:
        print(f'   结果：进程已退出（用了 {gone_after:.2f}s，退出码 {exit_code(pid)}）')
        leftover = [p for p in processes() if p[1] == pid]
        print(f'   孤儿子进程：{[f"{k[2]}({k[0]})" for k in leftover] or "无"}')
        return 0

    code = exit_code(pid)
    threads = next((x[3] for x in processes() if x[0] == pid), None)
    mods = modules_of(pid)
    print(f'   结果：进程**没退**（等了 {args.exit_timeout_ms / 1000.0:.0f}s）')
    print(f'   ExitCode={code}（{STILL_ACTIVE}=还活着；其它值=已进入退出流程却没退完，即僵尸）')
    print(f'   线程数={threads}  已映射模块={len(mods)}  仍映射的插件 dll='
          f'{[m for m in mods if m.lower().endswith(".dll") and "main" in m.lower()] or "无"}')
    print(f'   可见窗口={visible_windows(pid) or "无（窗口已销毁）"}')
    print('   处置：taskkill 对这种进程无效，用')
    print(f'         Get-CimInstance Win32_Process -Filter "ProcessId={pid}" | Invoke-CimMethod -MethodName Terminate')
    return 1


if __name__ == '__main__':
    sys.exit(main())
