# -*- coding: utf-8 -*-
"""修复 Ninja 构建树的 MSVC 头文件依赖追踪失效。

背景：本机 VS 18 只安装了中文(2052) clui.dll，cl.exe 的 /showIncludes
输出为 GBK 编码的 "注意: 包含文件:  "（冒号后两个空格）。CMake 重新
configure 时会把 msvc_deps_prefix 写成 UTF-8 编码的同一文本，与 cl
实际输出的 GBK 字节不一致，ninja 解析不到依赖 → 改头文件不触发重编。

用法（每次 cmake 重新 configure 之后跑一次）：
    python misc/fix-ninja-deps.py
    python misc/fix-ninja-deps.py <build目录>   # 默认 build/windows-ninja

原理：把 CMakeFiles/rules.ninja 里的 msvc_deps_prefix 从 UTF-8 字节
替换为 cl 实际输出的 GBK 字节。幂等，重复执行无副作用。

根治方案（可选）：在 Visual Studio Installer 里给 VS 18 补装英语语言包，
然后删掉 build 目录重新 configure，cl 将输出纯 ASCII 的
"Note: including file: "，此脚本即不再需要。
"""

import sys
from pathlib import Path

GBK_PREFIX = "注意: 包含文件: ".encode("gbk")          # D7A2 D2E2 3A 20 ...
UTF8_PREFIX = "注意: 包含文件: ".encode("utf-8")        # E6B3A8 E6848F ...


def fix(build_dir: Path) -> bool:
    rules = build_dir / "CMakeFiles" / "rules.ninja"
    if not rules.exists():
        print(f"[跳过] 找不到 {rules}（未用 Ninja 生成器？）")
        return False
    data = rules.read_bytes()
    needle = b"msvc_deps_prefix = " + UTF8_PREFIX
    if needle in data:
        rules.write_bytes(data.replace(needle, b"msvc_deps_prefix = " + GBK_PREFIX))
        print(f"[修复] {rules}: msvc_deps_prefix UTF-8 -> GBK")
        return True
    if b"msvc_deps_prefix = " + GBK_PREFIX in data:
        print(f"[已就绪] {rules}: 前缀已是 GBK，无需修改")
        return True
    # 前缀既不是 UTF-8 也不是预期 GBK——可能是纯 ASCII（已装英文语言包）
    import re
    m = re.search(rb"msvc_deps_prefix[^\r\n]*", data)
    print(f"[跳过] 前缀为 {m.group(0)!r}，非中文本地化输出，无需处理")
    return True


if __name__ == "__main__":
    repo = Path(__file__).resolve().parent.parent
    build = Path(sys.argv[1]) if len(sys.argv) > 1 else repo / "build" / "windows-ninja"
    sys.exit(0 if fix(build) else 1)
