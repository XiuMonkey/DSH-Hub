#pragma once

// ------------------------------------------------------------------
// LoadMoreButton.h
// ------------------------------------------------------------------
// 对话历史顶部的“加载更多”按钮。
// ------------------------------------------------------------------

#include <QPushButton>

class QEvent;

class LoadMoreButton : public QPushButton
{
	Q_OBJECT

public:
	explicit LoadMoreButton(QWidget* parent = nullptr);

protected:
	// 语言切换后重新设置按钮文案
	void changeEvent(QEvent* event) override;
};
