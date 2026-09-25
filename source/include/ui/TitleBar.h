#pragma once

// 自绘标题栏（只画，不做决定）：按钮只发"意图"信号，动作由 common/WindowFrame 落地，外观走 QSS。
// ⚠️ objectName 固定 windowTitleBar；窗口按钮带动态属性 dshWindowControl=true，命中测试须排除。

#include <QWidget>

class QEvent;
class QLabel;
class QPushButton;

class TitleBar : public QWidget
{
	Q_OBJECT

public:
	static constexpr int kHeight = 42;
	explicit TitleBar(QWidget* parent = nullptr);

	void setMaximizedState(bool maximized);

signals:
	void minimizeRequested();
	void maximizeRestoreRequested();
	void closeRequested();

protected:
	// 语言切换后重设按钮文案
	void changeEvent(QEvent* event) override;

private:
	void retranslateUi();

	QLabel* m_logoLabel = nullptr;
	QPushButton* m_minimizeButton = nullptr;
	QPushButton* m_maximizeButton = nullptr;
	QPushButton* m_closeButton = nullptr;
	bool m_maximized = false;
};
