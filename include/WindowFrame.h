#pragma once

// ------------------------------------------------------------------
// WindowFrame.h
// ------------------------------------------------------------------
// 无边框窗口的“窗口级功能逻辑”，不含任何绘制：原生样式位与 DWM 圆角、
// WM_NCCALCSIZE / WM_NCHITTEST 处理、边缘缩放热区、标题栏拖动区判定、
// 最大化时的工作区补偿、半透明遮罩的覆盖范围与显隐时机、窗口动作。
//
// 画的部分不在这里：
//   * 标题栏控件与窗口按钮 → src/ui/TitleBar.cpp
//   * 圆角 + 1px 描边       → resources/styles/main-window.qss（#dshhubCentral）
//
// 与控件之间的两条约定（约定写在控件侧，这里只做判定）：
//   * 标题栏控件的 objectName 必须是 windowTitleBar（拖动区与遮罩范围都按它算）
//   * 窗口按钮控件要带动态属性 dshWindowControl=true —— 命中测试时排除它们，
//     否则鼠标落在按钮上会被系统当成标题栏拖动，按钮永远收不到点击
// ------------------------------------------------------------------

#include <QRect>

class QByteArray;
class QLayout;
class QPoint;
class QWidget;

namespace WindowFrame
{
	// 补回 Qt::FramelessWindowHint 摘掉的 WS_THICKFRAME / WS_CAPTION 等样式位，
	// 并关掉 Windows 11 的系统圆角（半径 8px，会把自绘圆角的四个角裁掉），
	// 只保留系统投影、贴边吸附、右键系统菜单等原生行为。
	// 原生窗口创建之后调用；幂等（样式位已补过就直接返回）。
	void applyNativeStyle(QWidget* window);

	// 消息钩子：窗口的 nativeEvent 原样转进来。返回 true 表示已处理并写好了 result。
	bool handleNativeMessage(QWidget* window, const QWidget* titleBar,
		const QByteArray& eventType, void* message, qintptr* result);

	// 是否贴屏幕边缘（最大化/全屏）：此时不留圆角描边，也不给边缘缩放热区
	bool isEdgeToEdge(const QWidget* window);

	// 按“是否贴屏幕边缘”切换可见面的圆角描边状态：给可见面打上 maximized
	// 动态属性，由 main-window.qss 里 #dshhubCentral[maximized="true"] 那条规则生效
	// （动态属性变化不会自动重新匹配选择器，这里顺带 repolish）
	void applyBorderState(QWidget* surface, bool edgeToEdge);

	// 最大化/全屏时把“系统多给的那一圈”补进内容边距（非最大化时归零）
	void applyMaximizedContentInset(const QWidget* window, QLayout* contentLayout);

	// 边框的“界面收尾”组合动作：按当前是否贴屏幕边缘，依次切可见面的圆角描边
	// 状态、补最大化时的内容边距。返回是否贴屏幕边缘——调用方据此刷新标题栏
	// 按钮的“最大化/还原”图标（按钮是控件，本文件不碰控件类型）。
	// 顺序固定在这里，省得每个调用方各写一遍。
	// surface 传窗口的可见面（主窗口即 centralWidget()）。
	bool applyFrameStyle(QWidget* window, QWidget* surface);

	// 半透明遮罩（设置/插件/扩展管理）的覆盖范围：标题栏以下的内容区。
	// 遮罩铺满客户区会把自绘标题栏一起盖住，窗口按钮就点不动了。
	QRect overlayRect(const QWidget* host);

	// ------------------------------------------------------------------
	// 半透明遮罩的显示与隐藏（遮罩本身怎么画在 popups.qss 的 #windowScrim 里）
	// ------------------------------------------------------------------
	// 一个宿主窗口只留一层遮罩：设置 / 插件市场 / 扩展管理 / 工具过滤共用，
	// 同一个宿主第二次 showOverlay() 直接复用已经建好的那个控件（原来四个弹窗
	// 各 new 一个，同时开着就是两层半透明控件一起刷）。
	//
	// owner 传调用方 this：按调用方记名，多个弹窗同时开着只画一层，
	// 最后一个 release 的才真正隐藏。所以 show/hide 必须成对（同一个 owner）。
	//
	// 两个函数都在末尾补一次"同步重绘"，让遮罩和弹窗落在同一帧：
	//   * 显示：遮罩是宿主主窗口的子控件，光 show() 要等宿主下一帧才上屏，
	//     而弹窗是独立顶层窗口、show() 立刻呈现 —— 不补这次重绘，观感就是
	//     "弹窗先出、遮罩后到"；
	//   * 隐藏：同理。hide() 只是把这片区域标脏等宿主下一帧，而弹窗的 hide()
	//     是立刻上屏的 —— 不补这次重绘，就是"弹窗先没了、遮罩还留一拍"。
	// 这两处原来用的是 QCoreApplication::processEvents()：那等于把整个事件循环
	// 泵一遍（还带重入），慢且不可控，这里换成定向重绘。
	void showOverlay(QWidget* host, const void* owner);
	void hideOverlay(QWidget* host, const void* owner);

	// 宿主 resize / 移动后重新铺满（遮罩没显示时空操作）
	void syncOverlay(QWidget* host);

	// 窗口动作：标题栏按钮只发意图，动作在这里落地
	void minimize(QWidget* window);
	void toggleMaximize(QWidget* window);
	void closeWindow(QWidget* window);
}
