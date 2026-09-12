#pragma once

// ------------------------------------------------------------------
// TitleBar.h
// ------------------------------------------------------------------
// 自绘标题栏（**只画，不做决定**）：左侧应用图标，右侧最小化 / 最大化 /
// 关闭三个按钮。按钮被点击时只发出“意图”信号，具体动作（最小化/最大化/
// 关闭窗口）由 common/WindowFrame 落地，接线在 DSHHub 构造函数。
//
// 三个按钮是普通 QPushButton，**外观全部走 QSS**（resources/styles/base.qss
// 里的 #windowMinButton / #windowMaxButton / #windowCloseButton 规则），
// 这里只负责换字形（最大化 ↔ 还原）与发意图信号。
//
// 拖动窗口、双击最大化、贴边吸附、右键系统菜单都不在这里实现：命中测试
// 把本控件覆盖的区域（按钮除外）当成系统标题栏交回系统处理，
// 见 common/WindowFrame.cpp。
//
// 两条对外约定：
//   * objectName 固定为 windowTitleBar（拖动区/遮罩范围按它算）
//   * 三个窗口按钮带动态属性 dshWindowControl=true（命中测试时排除，
//     否则点按钮会变成拖窗口）
// ------------------------------------------------------------------

#include <QWidget>

class QEvent;
class QLabel;
class QPushButton;

class TitleBar : public QWidget
{
	Q_OBJECT

public:
	// 标题栏高度（Main.cpp 据此换算窗口高度，见 buildUi）
	static constexpr int kHeight = 42;

	explicit TitleBar(QWidget* parent = nullptr);

	// 由宿主在窗口状态变化时调用（本控件不监听宿主，保持“只画不做决定”）
	void setMaximizedState(bool maximized);

signals:
	void minimizeRequested();
	void maximizeRestoreRequested();
	void closeRequested();

protected:
	// 语言切换（QEvent::LanguageChange）后重新设置按钮文案
	void changeEvent(QEvent* event) override;

private:
	void retranslateUi();

	QLabel* m_logoLabel = nullptr;
	QPushButton* m_minimizeButton = nullptr;
	QPushButton* m_maximizeButton = nullptr;
	QPushButton* m_closeButton = nullptr;
	// 记住最大化状态：语言切换后要按同一状态重设“最大化/还原”的提示与字形
	bool m_maximized = false;
};
