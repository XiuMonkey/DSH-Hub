#!/usr/bin/env bash
# 诊断 VS / MSBuild 报出的「一屏假错误」。
#
# 背景：在 VS 里构建 DSH Hub 时，有时会刷出几十上百条
#   未定义标识符 "m_xxx" / 不允许使用不完整的类型 "DSHHub" / 应输入";" /
#   此声明没有存储类或类型说明符 / "this"只能用于非静态成员函数内部
# 这些**几乎全是症状而不是病因**。本脚本按「先查真因、再验真错」的顺序给结论。
#
# 用法（工具层 bash 的 PATH 可能是空的，用绝对路径调解释器）：
#   "<PortableGit>/bin/bash.exe" tools/diagnose-build.sh
set -u

export PATH="/c/Users/Playe/.workbuddy/binaries/PortableGit/versions/1.2.0/usr/bin:/c/Windows/System32:/c/Windows:/c/Windows/System32/WindowsPowerShell/v1.0"

MSVC_WIN="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231"
KITS_WIN="C:/Program Files (x86)/Windows Kits/10"
SDK_VER="10.0.26100.0"
MSVC_MSYS="/c/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231"
KITS_MSYS="/c/Program Files (x86)/Windows Kits/10"

ROOT="/c/Users/Playe/Documents/DSH hub/DSH Hub"
EXE="$ROOT/x64/Release/DSH Hub.exe"
INT="$ROOT/DSH Hub/x64/Release"

echo "=================================================="
echo " 步骤 1：有没有实例在跑？（最常见真因）"
echo "=================================================="
running=$(MSYS_NO_PATHCONV=1 tasklist 2>/dev/null | tr -d '\r' | grep -i "DSH Hub" || true)
if [ -n "$running" ]; then
	echo "  ⚠ 发现正在运行的实例："
	echo "$running" | sed 's/^/    /'
	echo
	echo "  → 这会锁住 $EXE，链接失败后 MSBuild 会放大成满屏假错误。"
	echo "  → 处理：关闭它，或直接跑 tools/rebuild.sh（它会先关再编）。"
else
	echo "  ✓ 没有实例在跑。"
fi

echo
echo "=================================================="
echo " 步骤 2：exe 是否可写？（锁的硬证据）"
echo "=================================================="
if [ -f "$EXE" ]; then
	if mv "$EXE" "$EXE.locktest" 2>/dev/null; then
		mv "$EXE.locktest" "$EXE"
		echo "  ✓ 可以改名 → exe 没被占用。"
	else
		echo "  ⚠ 无法改名 → exe 仍被某个进程占用（可能不是 DSH Hub 本身，"
		echo "    例如资源管理器缩略图预览、杀毒扫描）。"
	fi
else
	echo "  (尚未生成 $EXE)"
fi

echo
echo "=================================================="
echo " 步骤 3：产物是否比源码新？（是否真的编过了）"
echo "=================================================="
newest_src=$(ls -t "$ROOT"/src/core/*.cpp "$ROOT"/src/chat/*.cpp "$ROOT"/include/*.h 2>/dev/null | head -1)
echo "  最新源码 : $(ls -la "$newest_src" 2>/dev/null | awk '{print $6,$7,$8,$9}')"
[ -f "$EXE" ] && echo "  exe      : $(ls -la "$EXE" | awk '{print $6,$7,$8,$9}')"
[ -f "$INT/DSHHub.obj" ] && echo "  DSHHub.obj: $(ls -la "$INT/DSHHub.obj" | awk '{print $6,$7,$8,$9}')"
echo "  （obj/exe 时间晚于源码 = 已成功编译链接过）"

echo
echo "=================================================="
echo " 步骤 4：真做一次语法检查（这才是「有没有真错」）"
echo "=================================================="
export INCLUDE="$MSVC_WIN/include;$KITS_WIN/Include/$SDK_VER/ucrt;$KITS_WIN/Include/$SDK_VER/shared;$KITS_WIN/Include/$SDK_VER/um;$KITS_WIN/Include/$SDK_VER/winrt;$KITS_WIN/Include/$SDK_VER/cppwinrt"
export LIB="$MSVC_WIN/lib/x64;$KITS_WIN/Lib/$SDK_VER/ucrt/x64;$KITS_WIN/Lib/$SDK_VER/um/x64"
export PATH="$MSVC_MSYS/bin/Hostx64/x64:$KITS_MSYS/bin/$SDK_VER/x64:$PATH"

QT="D:/Qt/6.11.2/msvc2022_64/include"
total_err=0
for f in "$ROOT"/src/core/DSHHub.cpp "$ROOT"/src/core/MessageHost.cpp; do
	name=$(basename "$f")
	# 注意：cl 的输出含中文，grep 可能把它当二进制 → 必须加 -a，
	# 否则 "Binary file (standard input) matches" 会被误判成 error。
	out=$(cl.exe /nologo /Zs /std:c++17 /Zc:__cplusplus /permissive- /EHsc /utf-8 \
		/I"$ROOT/include" /I"$QT" /I"$QT/QtCore" /I"$QT/QtGui" \
		/I"$QT/QtWidgets" /I"$QT/QtNetwork" /I"$QT/QtWebSockets" \
		/DUNICODE /D_UNICODE /DWIN32 /D_WINDOWS /DQT_NO_DEBUG \
		"$f" 2>&1 | grep -a -E "error C[0-9]+|error LNK|fatal error" || true)
	if [ -z "$out" ]; then
		echo "  ✓ $name 编译干净（0 error）"
	else
		n=$(echo "$out" | wc -l)
		total_err=$((total_err + n))
		echo "  ✗ $name 有 $n 条 error："
		echo "$out" | head -10 | sed 's/^/      /'
	fi
done

echo
echo "=================================================="
if [ "$total_err" -eq 0 ]; then
	echo " 结论：源码没有真错误。你看到的那些是级联假错，"
	echo "       按步骤 1 关掉实例后重新构建即可。"
else
	echo " 结论：出现 $total_err 条真实 error，需要按上面的位置修代码。"
fi
echo "=================================================="
