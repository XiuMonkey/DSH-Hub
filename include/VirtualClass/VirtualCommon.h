#pragma once
#include <qplugin.h>
#include <qboxlayout.h>

// 接口里的参数只按指针传递，前向声明就够（宿主与插件各编一份，谁都不要
// 依赖对方 exe 里的符号）
class QWidget;

class VirtualTopBar {
public:
	virtual ~VirtualTopBar() = default;
	virtual QHBoxLayout* GetLayout() = 0;
};

class VirtualTheme {
public:
	virtual ~VirtualTheme() = default;
	virtual void ExternalApplyToWindow(QWidget* window) = 0;
	// 重新读一遍本地样式文件（exe 同目录 styles/*.qss 与 theme-*.json）并把新样式表
	// 挂回所有顶层窗口；返回"重载后当前样式表非空"（false 基本等于文件没读到，
	// 便于插件覆盖完文件后自检）。插件覆盖完这些文件后调它即可。
	//
	// ⚠️ 虚方法只许**在末尾追加**：宿主的 vtable 槽位 = 声明顺序，而插件 DLL 是
	// 独立编译的。在末尾追加时老插件照旧能用（它只碰得到前面的槽位）；插到中间
	// 会让已发布的插件全部调错位置。
	virtual bool ExternalReloadStyles() = 0;
};

Q_DECLARE_INTERFACE(VirtualTopBar, "com.DSH_HUB.VirtualCommon/1.0")

// IID 是宿主与插件之间唯一的约定：插件侧 obj->qt_metacast(IID) 用的就是这个串，
// 两边 include 同一份头文件即自洽。改它 = 改接口 ABI（旧插件会静默转成功不了）。
Q_DECLARE_INTERFACE(VirtualTheme, "com.DSH_HUB.VirtualTheme/1.0")

// ------------------------------------------------------------------
// VirtualWindow —— 宿主主窗口对外暴露的"窗口级"能力
// ------------------------------------------------------------------
// 目前只开一件事：把扩展自己的顶层窗口当成宿主弹窗（铺遮罩 + 居中显示）。
// 与 VirtualTheme 同一套路：宿主类实现它（Q_INTERFACES），插件把注册表里取到的
// mainWindow 转成 VirtualWindow* —— 转换走 obj->qt_metacast(IID)（跨边界只传字符串，
// 插件侧零宿主符号），调用走 vtable（同样不产生外部符号）。
class VirtualWindow {
public:
	virtual ~VirtualWindow() = default;

	// 铺遮罩 + 把 popup 居中并显示，两步背靠背完成 —— 与宿主自己的设置 / 插件市场 /
	// 扩展管理弹窗走同一条路径（WindowFrame::showOverlayWithPopup），所以不会出现
	// "遮罩先到、弹窗后到"。
	// ⚠️ 调用前 popup 的尺寸必须已经定好：宿主按自己的中心给它定位。
	virtual void ExternalShowOverlay(QWidget* popup) = 0;

	// 收遮罩。**必须与 ExternalShowOverlay 成对**：遮罩在宿主窗口上只留一层、
	// 按 owner 记名（见 WindowFrame.h），只有最后一个 release 的才真正隐藏。
	virtual void ExternalHideOverlay(QWidget* popup) = 0;
};

Q_DECLARE_INTERFACE(VirtualWindow, "com.DSH_HUB.VirtualWindow/1.0")
