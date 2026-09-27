#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从外部给"卡住/僵尸"的进程验尸：进程普查 + 残留线程栈回填模块与导出符号名。

为什么需要它：DSH Hub 关窗后偶尔会留下"无窗口、1 线程、main.dll 还被锁着"的僵尸进程，
任务管理器只给一个 PID，什么都说明不了。本脚本不注入、不调试，只用 kernel32/user32/psapi
的公开 API 读：

  1. 普查：每个匹配进程的 创建时间 / 线程数 / 退出码 / 句柄数 / GDI与USER对象数 / 映像路径。
     - 退出码 != 259(STILL_ACTIVE) 而地址空间和句柄还在 ⇒ 它"已经开始退出但没退完"。
     - GDI/USER 掉到个位数 ⇒ 它的窗口全没了（不是"还开着但没响应"）。
  2. 验尸：SuspendThread + GetThreadContext 取残留线程的 RIP，ReadProcessMemory 读一页栈，
     把里面每个像返回地址的 8 字节点回 模块+偏移，再解析该模块的导出表点回函数名
     （Qt 的 DLL 导出 C++ 修饰名，所以能直接看到 QProcess 析构、QWindowsPipeReader 这类名字）。

用法（Windows，任意 Python 3.8+，无需第三方包）：
    python misc/tools/stuck-proc-probe.py "DSH Hub"          # 普查
    python misc/tools/stuck-proc-probe.py --stack 34152      # 给某个 PID 验尸

注意：验尸会短暂 SuspendThread 再恢复；对已经卡死的线程无副作用。
"""
import ctypes
import ctypes.wintypes as wt
import datetime
import struct
import sys

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
u32 = ctypes.WinDLL('user32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)

k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD,
                                       ctypes.POINTER(wt.DWORD), wt.DWORD]
psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_wchar_p, wt.DWORD]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPTHREAD = 0x4
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
THREAD_GET_CONTEXT = 0x0008
THREAD_QUERY_INFORMATION = 0x0040
THREAD_SUSPEND_RESUME = 0x0002
STILL_ACTIVE = 259
LIST_MODULES_ALL = 0x03

# 这些模块的导出表要解析：ntdll/kernelbase 看系统等待，Qt6Core 看 Qt 内部，
# 扩展自身的 main.dll 看是谁在卸载它。
SYMBOL_MODULES = ('ntdll.dll', 'kernel32.dll', 'kernelbase.dll', 'ucrtbase.dll', 'msvcp140.dll',
                  'qt6core.dll', 'qt6widgets.dll', 'qt6network.dll', 'main.dll')


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD),
                ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long),
                ("tpDeltaPri", ctypes.c_long), ("dwFlags", wt.DWORD)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", wt.DWORD),
                ("EntryPoint", ctypes.c_void_p)]


class M128A(ctypes.Structure):
    _fields_ = [("Low", ctypes.c_ulonglong), ("High", ctypes.c_longlong)]


# x64 CONTEXT 的前 6 个字段是 6 个独立的 ULONGLONG（不是 3 对），写错一位
#    整个结构就错位，RIP 会读出 0 —— 踩过。
class CONTEXT(ctypes.Structure):
    _fields_ = [("P1Home", ctypes.c_ulonglong), ("P2Home", ctypes.c_ulonglong),
                ("P3Home", ctypes.c_ulonglong), ("P4Home", ctypes.c_ulonglong),
                ("P5Home", ctypes.c_ulonglong), ("P6Home", ctypes.c_ulonglong),
                ("ContextFlags", wt.DWORD), ("MxCsr", wt.DWORD),
                ("SegCs", wt.WORD), ("SegDs", wt.WORD), ("SegEs", wt.WORD), ("SegFs", wt.WORD),
                ("SegGs", wt.WORD), ("SegSs", wt.WORD), ("EFlags", wt.DWORD),
                ("Dr0", ctypes.c_ulonglong), ("Dr1", ctypes.c_ulonglong),
                ("Dr2", ctypes.c_ulonglong), ("Dr3", ctypes.c_ulonglong),
                ("Dr6", ctypes.c_ulonglong), ("Dr7", ctypes.c_ulonglong),
                ("Rax", ctypes.c_ulonglong), ("Rcx", ctypes.c_ulonglong),
                ("Rdx", ctypes.c_ulonglong), ("Rbx", ctypes.c_ulonglong),
                ("Rsp", ctypes.c_ulonglong), ("Rbp", ctypes.c_ulonglong),
                ("Rsi", ctypes.c_ulonglong), ("Rdi", ctypes.c_ulonglong),
                ("R8", ctypes.c_ulonglong), ("R9", ctypes.c_ulonglong),
                ("R10", ctypes.c_ulonglong), ("R11", ctypes.c_ulonglong),
                ("R12", ctypes.c_ulonglong), ("R13", ctypes.c_ulonglong),
                ("R14", ctypes.c_ulonglong), ("R15", ctypes.c_ulonglong),
                ("Rip", ctypes.c_ulonglong),
                ("FltSave", ctypes.c_byte * 512), ("VectorRegister", M128A * 26),
                ("VectorControl", ctypes.c_ulonglong), ("DebugControl", ctypes.c_ulonglong),
                ("LastBranchToRip", ctypes.c_ulonglong), ("LastBranchFromRip", ctypes.c_ulonglong),
                ("LastExceptionToRip", ctypes.c_ulonglong),
                ("LastExceptionFromRip", ctypes.c_ulonglong)]


def ft_dt(ft):
    if not (ft.dwLowDateTime or ft.dwHighDateTime):
        return None
    v = (ft.dwHighDateTime << 32) | ft.dwLowDateTime
    return datetime.datetime(1601, 1, 1) + datetime.timedelta(microseconds=v // 10) \
        + datetime.timedelta(hours=8)


def proc_state(pid):
    """(exitCode, 句柄数, GDI, USER, 创建, 退出时刻, CPU秒)"""
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return None
    code, hc = wt.DWORD(), wt.DWORD()
    k32.GetExitCodeProcess(h, ctypes.byref(code))
    k32.GetProcessHandleCount(h, ctypes.byref(hc))
    ct, et, kt, ut = (wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME())
    k32.GetProcessTimes(h, ctypes.byref(ct), ctypes.byref(et), ctypes.byref(kt), ctypes.byref(ut))
    cpu = (((kt.dwHighDateTime << 32) | kt.dwLowDateTime) +
           ((ut.dwHighDateTime << 32) | ut.dwLowDateTime)) / 1e7
    gdi = u32.GetGuiResources(h, 0)
    usr = u32.GetGuiResources(h, 1)
    k32.CloseHandle(h)
    return code.value, hc.value, gdi, usr, ft_dt(ct), ft_dt(et), cpu


def image_path(pid):
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return "?"
    buf = ctypes.create_unicode_buffer(1024)
    size = wt.DWORD(1024)
    ok = k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size))
    k32.CloseHandle(h)
    return buf.value if ok else "?"


def census(pattern):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32W()
    pe.dwSize = ctypes.sizeof(pe)
    rows = []
    ok = k32.Process32FirstW(snap, ctypes.byref(pe))
    while ok:
        if pattern.lower() in pe.szExeFile.lower():
            rows.append((pe.th32ProcessID, pe.th32ParentProcessID, pe.szExeFile, pe.cntThreads))
        ok = k32.Process32NextW(snap, ctypes.byref(pe))
    k32.CloseHandle(snap)
    if not rows:
        print("没有匹配 %r 的进程" % pattern)
        return
    print("PID      PPID   线程  退出码      句柄    GDI   USER  创建       状态    映像")
    for pid, ppid, name, cnt in sorted(rows):
        st = proc_state(pid)
        if not st:
            print("%-8d %-7d %-5d %s" % (pid, ppid, cnt, "<打不开>"))
            continue
        code, hc, gdi, usr, born, exited, cpu = st
        if code == STILL_ACTIVE:
            verdict = "运行中"
        elif gdi < 40 and hc > 50:
            verdict = "★僵尸（已退出但地址空间/句柄没释放）"
        else:
            verdict = "已退出"
        print("%-8d %-7d %-5d 0x%-8X %-6d %-5d %-5d %-10s %-6s %s"
              % (pid, ppid, cnt, code, hc, gdi, usr,
                 born.strftime('%H:%M:%S') if born else "?", verdict, image_path(pid)))


def exports_of(path):
    """{rva: 导出名}。Qt 的 DLL 导出 C++ 修饰名，所以能点回 QProcess 析构这类符号。"""
    try:
        data = open(path, 'rb').read()
    except OSError:
        return {}
    e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b'PE\0\0':
        return {}
    coff = e_lfanew + 4
    nsec, opt_size = struct.unpack_from('<HH', data, coff + 2)[0], struct.unpack_from('<H', data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from('<H', data, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)   # 数据目录起始：PE32+ 112，PE32 96
    exp_rva = struct.unpack_from('<I', data, dd)[0]
    if not exp_rva:
        return {}
    secs = []
    for i in range(nsec):
        off = opt + opt_size + i * 40
        va, vsz = struct.unpack_from('<II', data, off + 12)
        raw, rawsz = struct.unpack_from('<II', data, off + 20)
        secs.append((va, vsz, raw, rawsz))

    def rva2off(rva):
        for va, vsz, raw, rawsz in secs:
            if va <= rva < va + max(vsz, rawsz):
                return raw + (rva - va)
        return None

    eo = rva2off(exp_rva)
    if eo is None:
        return {}
    nname = struct.unpack_from('<I', data, eo + 24)[0]
    afun, aname = struct.unpack_from('<II', data, eo + 28)[0], struct.unpack_from('<II', data, eo + 32)[0]
    ao, an = rva2off(afun), rva2off(aname)
    if ao is None or an is None:
        return {}
    out = {}
    for i in range(nname):
        no = rva2off(struct.unpack_from('<I', data, an + i * 4)[0])
        if no is None:
            continue
        end = data.index(b'\0', no)
        out[struct.unpack_from('<I', data, ao + i * 4)[0]] = data[no:end].decode('ascii', 'replace')
    return out


def modules_of(h):
    arr = (wt.HMODULE * 1024)()
    need = wt.DWORD()
    res = []
    if not psapi.EnumProcessModulesEx(h, arr, ctypes.sizeof(arr), ctypes.byref(need), LIST_MODULES_ALL):
        return res
    for i in range(need.value // ctypes.sizeof(wt.HMODULE)):
        mi = MODULEINFO()
        if not psapi.GetModuleInformation(h, ctypes.c_void_p(arr[i]), ctypes.byref(mi), ctypes.sizeof(mi)):
            continue
        buf = ctypes.create_unicode_buffer(1024)
        psapi.GetModuleFileNameExW(h, ctypes.c_void_p(arr[i]), buf, 1024)
        res.append((mi.lpBaseOfDll or 0, mi.SizeOfImage, buf.value))
    return res


def resolve(mods, addr):
    for base, size, path in mods:
        if base <= addr < base + size:
            return path, addr - base
    return None, None


def nearest(exp, rva, limit=0x4000):
    best = None
    for frva, nm in exp.items():
        if frva <= rva and (best is None or frva > best[0]):
            best = (frva, nm)
    if best and rva - best[0] <= limit:
        return best
    return None


def forensics(pid, depth=4096):
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        print("OpenProcess 失败 err=%d" % ctypes.get_last_error())
        return
    mods = modules_of(h)
    exp = {}
    for base, size, path in mods:
        if path.lower().endswith(SYMBOL_MODULES):
            e = exports_of(path)
            if e:
                exp[path] = e
    print("=== PID %d：模块 %d 个，可解析符号的 %d 个 ===" % (pid, len(mods), len(exp)))

    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    tids = []
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            tids.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)

    def label(addr):
        path, off = resolve(mods, addr)
        if not path:
            return None
        name = path.split('\\')[-1]
        s = "%s+0x%X" % (name, off)
        n = nearest(exp.get(path, {}), off)
        return s + ("  ~%s+0x%X" % (n[1], off - n[0]) if n else "")

    for tid in tids:
        th = k32.OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME, False, tid)
        if not th:
            print("\nTID %d 打不开 err=%d" % (tid, ctypes.get_last_error()))
            continue
        k32.SuspendThread(th)
        ctx = CONTEXT()
        ctx.ContextFlags = 0x0010000B
        got = k32.GetThreadContext(th, ctypes.byref(ctx))
        rip, rsp = ctx.Rip, ctx.Rsp
        buf = ctypes.create_string_buffer(depth)
        nread = ctypes.c_size_t()
        k32.ReadProcessMemory(h, ctypes.c_void_p(rsp), buf, depth, ctypes.byref(nread))
        k32.ResumeThread(th)
        k32.CloseHandle(th)
        print("\nTID %d   RSP=0x%X" % (tid, rsp))
        if got:
            print("  当前 RIP: %s" % (label(rip) or ("0x%X" % rip)))
        print("  栈上返回地址候选（低 -> 高，即由内到外）：")
        for i in range(nread.value // 8):
            v = struct.unpack_from('<Q', buf.raw, i * 8)[0]
            if not (0x10000 < v < 0x7FFFFFFFFFFF):
                continue
            lab = label(v)
            if lab:
                print("    +%04X  %s" % (i * 8, lab))
    k32.CloseHandle(h)


if __name__ == '__main__':
    args = sys.argv[1:]
    if not args:
        print(__doc__)
    elif args[0] == '--stack':
        for p in args[1:]:
            forensics(int(p))
    else:
        census(args[0])
