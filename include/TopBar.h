#pragma once

// ------------------------------------------------------------------
// TopBar.h
// ------------------------------------------------------------------
// 对话顶部栏：白色圆角 + 细边框，宽度与对话栏一致。
// ------------------------------------------------------------------

#include <QWidget>

class QLabel;

class TopBar : public QWidget
{
	Q_OBJECT

public:
	explicit TopBar(QWidget* parent = nullptr);

	void setTitle(const QString& title);

private:
	QLabel* m_titleLabel = nullptr;
};
