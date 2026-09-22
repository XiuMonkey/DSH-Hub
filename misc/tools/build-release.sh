#!/usr/bin/env bash
# 在 bash（非 cmd.exe）下用 MSVC + Ninja 构建 DSH Hub Release。
# 背景：工具层会拦截 cmd.exe，所以 vcvars64.bat 那条常规路子走不通；
# 这里手工导出 MSVC 环境变量后再调 cmake --build。
#
# 用法（工具层的 bash 初始 PATH 可能是空的，所以用绝对路径调解释器）：
#   "<PortableGit>/bin/bash.exe" misc/tools/build-release.sh [target]
#   target 默认 dshhub；也可传 dshhub_tests 等。多余参数原样转给 cmake。
set -u

export PATH="/c/Users/Playe/.workbuddy/binaries/PortableGit/versions/1.2.0/usr/bin:/c/Windows/System32:/c/Windows:/c/Windows/System32/WindowsPowerShell/v1.0"

ROOT="/c/Users/Playe/Documents/DSH hub/DSH Hub"
BUILD="$ROOT/build/windows-ninja"
# cmake.exe 是 Windows 程序，只认 Windows 形式的路径 —— 不能把 MSYS 的 /c/... 传给它
BUILD_WIN="C:/Users/Playe/Documents/DSH hub/DSH Hub/build/windows-ninja"

MSVC_WIN="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231"
KITS_WIN="C:/Program Files (x86)/Windows Kits/10"
SDK_VER="10.0.26100.0"

MSVC_MSYS="/c/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231"

# INCLUDE / LIB 用正斜杠 + ';' 分隔（cl 接受正斜杠）
export INCLUDE="$MSVC_WIN/include;$KITS_WIN/Include/$SDK_VER/ucrt;$KITS_WIN/Include/$SDK_VER/shared;$KITS_WIN/Include/$SDK_VER/um;$KITS_WIN/Include/$SDK_VER/winrt;$KITS_WIN/Include/$SDK_VER/cppwinrt"
export LIB="$MSVC_WIN/lib/x64;$KITS_WIN/Lib/$SDK_VER/ucrt/x64;$KITS_WIN/Lib/$SDK_VER/um/x64"

# PATH 里这些必须是 MSYS 形式（带盘符的 C:/... 在 bash 的 PATH 里不生效）
export PATH="$MSVC_MSYS/bin/Hostx64/x64:/d/Qt/Tools/Ninja:/d/Qt/Tools/CMake_64/bin:/d/Qt/6.11.2/msvc2022_64/bin:$PATH"

TARGET="${1:-dshhub}"
shift || true
cd "$BUILD" || exit 1
cmake --build "$BUILD_WIN" --config Release --target "$TARGET" "$@"
