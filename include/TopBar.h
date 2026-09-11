#pragma once

// ------------------------------------------------------------------
// TopBar.h
// ------------------------------------------------------------------
// 对话顶部栏：白色圆角 + 细边框，宽度与对话栏一致。
// ------------------------------------------------------------------

#include <QString>
#include <QWidget>

class QEvent;
class QLabel;

class TopBar : public QWidget
{
	Q_OBJECT

public:
	explicit TopBar(QWidget* parent = nullptr);

	void setTitle(const QString& title);

protected:
	// 语言切换后：占位标题“未命名会话”要跟着换
	void changeEvent(QEvent* event) override;

private:
	void retranslateUi();

	QLabel* m_titleLabel = nullptr;
	// 记住会话标题：空标题时显示的是可翻译的占位文案，切换语言要重算
	QString m_title;
};
