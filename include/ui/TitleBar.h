#pragma once

// 自绘标题栏（**只画，不做决定**）：左侧应用图标 + 产品名/版本，右侧最小化/最大化/关闭；按钮只发"意图"信号，动作由 common/WindowFrame 落地。
// 三个按钮是普通 QPushButton，外观全部走 QSS（#windowMinButton / #windowMaxButton / #windowCloseButton）；
// 两条硬约定：objectName 固定为 windowTitleBar（拖动区/遮罩范围按它算）；三个窗口按钮带动态属性 dshWindowControl=true（命中测试要排除，否则点按钮会变成拖窗口）。

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
	// 语言切换后要按同一状态重设最大化/还原的提示与字形
	bool m_maximized = false;
};
