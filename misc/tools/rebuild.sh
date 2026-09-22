#!/usr/bin/env bash
# 先关掉正在运行的 DSH Hub，再走 CMake + Ninja 构建。
#
# 为什么需要这个脚本：Windows 不允许覆盖正在运行的 exe。如果 app 还开着，
# 链接会以「无法打开文件 "...\x64\Release\DSH Hub.exe"」失败，而 MSBuild /
# VS 会把这次失败放大成满屏的假错误（未定义标识符 / 不完整的类型 / 应输入";"），
# 让人误以为源码坏了。关掉再编即可。
#
# 用法（工具层的 bash 初始 PATH 可能是空的，所以用绝对路径调解释器）：
#   "<PortableGit>/bin/bash.exe" misc/tools/rebuild.sh [target]
#   target 默认 dshhub；也可传 dshhub_tests。多余参数原样转给 cmake。
#
# 注意：关闭 → 构建 必须在同一次进程里完成。中途若 app 被拉起（VS 的调试启动、
# 或手动双击），锁会重新出现，链接又会失败。
set -u

export PATH="/c/Users/Playe/.workbuddy/binaries/PortableGit/versions/1.2.0/usr/bin:/c/Windows/System32:/c/Windows:/c/Windows/System32/WindowsPowerShell/v1.0"

ROOT="/c/Users/Playe/Documents/DSH hub/DSH Hub"
BUILD="$ROOT/build/windows-ninja"
BUILD_WIN="C:/Users/Playe/Documents/DSH hub/DSH Hub/build/windows-ninja"

# ------------------------------------------------------------------
# 1) 关掉可能锁住 exe 的实例
# ------------------------------------------------------------------
echo "[rebuild] 关闭正在运行的 DSH Hub ..."
for image in "DSH Hub.exe" "DSH Hub.Tests.exe"; do
	# MSYS_NO_PATHCONV=1：否则 /F 会被 MSYS 改写成 c:/... 路径
	MSYS_NO_PATHCONV=1 taskkill /F /IM "$image" >/dev/null 2>&1
done

# 等句柄真正释放（taskkill 返回后系统仍需极短时间回收映像）
for _ in 1 2 3 4 5 6 7 8 9 10; do
	still=$(MSYS_NO_PATHCONV=1 tasklist 2>/dev/null | tr -d '\r' | grep -ci "DSH Hub" || true)
	[ "$still" = "0" ] && break
	sleep 0.3
done

# 锁真的走了吗？能改名就说明没被占用
EXE="$ROOT/x64/Release/DSH Hub.exe"
if [ -f "$EXE" ]; then
	if mv "$EXE" "$EXE.locktest" 2>/dev/null; then
		mv "$EXE.locktest" "$EXE"
		echo "[rebuild] exe 未被占用，继续。"
	else
		echo "[rebuild] 警告：$EXE 仍被占用，链接会失败。" >&2
		echo "[rebuild] 请手动确认没有其他实例（或资源管理器预览）持有它。" >&2
	fi
fi

# ------------------------------------------------------------------
# 2) 构建（MSVC 环境手工导出，理由见 build-release.sh）
# ------------------------------------------------------------------
MSVC_WIN="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231"
KITS_WIN="C:/Program Files (x86)/Windows Kits/10"
SDK_VER="10.0.26100.0"
MSVC_MSYS="/c/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231"
KITS_MSYS="/c/Program Files (x86)/Windows Kits/10"

export INCLUDE="$MSVC_WIN/include;$KITS_WIN/Include/$SDK_VER/ucrt;$KITS_WIN/Include/$SDK_VER/shared;$KITS_WIN/Include/$SDK_VER/um;$KITS_WIN/Include/$SDK_VER/winrt;$KITS_WIN/Include/$SDK_VER/cppwinrt"
export LIB="$MSVC_WIN/lib/x64;$KITS_WIN/Lib/$SDK_VER/ucrt/x64;$KITS_WIN/Lib/$SDK_VER/um/x64"
# 注意：除了 MSVC 的 Hostx64/x64，还必须把 SDK 的 bin/x64 加进来，
# 否则 rc.exe 找不到，CMake 的编译器自检会直接失败。
export PATH="$MSVC_MSYS/bin/Hostx64/x64:$KITS_MSYS/bin/$SDK_VER/x64:/d/Qt/Tools/Ninja:/d/Qt/Tools/CMake_64/bin:/d/Qt/6.11.2/msvc2022_64/bin:$PATH"

TARGET="${1:-dshhub}"
shift || true

if [ ! -f "$BUILD_WIN/CMakeCache.txt" ]; then
	echo "[rebuild] 未配置过，先 configure ..."
	cmake -S "$ROOT/source/CMake" -B "$BUILD_WIN" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_MAKE_PROGRAM="D:/Qt/Tools/Ninja/ninja.exe" \
		-DCMAKE_PREFIX_PATH="D:/Qt/6.11.2/msvc2022_64" || exit 1
fi

cd "$BUILD" || exit 1
cmake --build "$BUILD_WIN" --config Release --target "$TARGET" "$@"
