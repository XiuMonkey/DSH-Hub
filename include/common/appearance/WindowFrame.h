#pragma once

// 无边框窗口的“窗口级功能逻辑”，不含任何绘制：原生样式位与 DWM 圆角、WM_NCCALCSIZE / WM_NCHITTEST、边缘缩放热区、标题栏拖动区判定、最大化时的工作区补偿、遮罩覆盖范围与显隐时机、窗口动作。
// 与控件的硬约定（判定在这里，控件侧只负责满足）：标题栏控件 objectName 必须是 windowTitleBar；窗口按钮控件必须带动态属性 dshWindowControl=true —— 否则鼠标落在按钮上会被系统当成标题栏拖动，按钮永远收不到点击。
// 绘制在 src/ui/TitleBar.cpp 与 resources/styles/main-window.qss（#dshhubCentral）。

#include <QRect>

class QByteArray;
class QLayout;
class QPoint;
class QWidget;

namespace WindowFrame
{
	// 补回 Qt::FramelessWindowHint 摘掉的 WS_THICKFRAME / WS_CAPTION 等样式位，并关掉 Windows 11 的系统圆角（半径 8px，会把自绘圆角的四角裁掉），只保留系统投影、贴边吸附、右键系统菜单；原生窗口创建之后调用，幂等。
	void applyNativeStyle(QWidget* window);

	// 消息钩子：窗口的 nativeEvent 原样转进来；返回 true 表示已处理并写好了 result。
	bool handleNativeMessage(QWidget* window, const QWidget* titleBar,
		const QByteArray& eventType, void* message, qintptr* result);

	// 是否贴屏幕边缘（最大化/全屏）：此时不留圆角描边，也不给边缘缩放热区。
	bool isEdgeToEdge(const QWidget* window);

	// 给可见面打上 maximized 动态属性，由 main-window.qss 的 #dshhubCentral[maximized="true"] 生效；动态属性变化不会自动重新匹配选择器，故这里顺带 repolish。
	void applyBorderState(QWidget* surface, bool edgeToEdge);

	// 最大化/全屏时把“系统多给的那一圈”补进内容边距（非最大化时归零）。
	void applyMaximizedContentInset(const QWidget* window, QLayout* contentLayout);

	// 边框的收尾组合动作（顺序固定在这里）：切圆角描边状态 + 补最大化内容边距；surface 传窗口的可见面（主窗口即 centralWidget()），返回值即 isEdgeToEdge()。
	bool applyFrameStyle(QWidget* window, QWidget* surface);

	// 半透明遮罩（设置/插件/扩展管理）的覆盖范围：标题栏以下的内容区 —— 遮罩铺满客户区会把自绘标题栏一起盖住，窗口按钮就点不动了。
	QRect overlayRect(const QWidget* host);

	// 半透明遮罩的显隐（怎么画在 popups.qss 的 #windowScrim）：一个宿主只留一层，owner 按调用方记名，最后一个 release 的才真正隐藏 —— show/hide 必须成对；隐藏时末尾补一次宿主同步重绘，让遮罩和弹窗落在同一帧。
	void hideOverlay(QWidget* host, const void* owner);

	// 铺遮罩 + 把弹窗居中并显示。必须合成一个函数：遮罩是宿主的子控件（同步重绘后立刻上屏），弹窗是独立顶层窗口 —— 只有两步在同一个事件循环轮次里完成，合成器才会放进同一帧；中间夹进耗时工作（建控件、拉数据）就会出现“遮罩先出、弹窗后到”，所以调用方的准备工作要在调本函数之前做完。
	void showOverlayWithPopup(QWidget* host, const void* owner, QWidget* popup);

	// 宿主 resize / 移动后重新铺满（遮罩没显示时空操作）。
	void syncOverlay(QWidget* host);

	void minimize(QWidget* window);
	void toggleMaximize(QWidget* window);
	void closeWindow(QWidget* window);
}
