#pragma once

// ------------------------------------------------------------------
// LoadMoreButton.h
// ------------------------------------------------------------------
// 对话历史顶部的“加载更多”按钮。
// ------------------------------------------------------------------

#include <QPushButton>

class LoadMoreButton : public QPushButton
{
    Q_OBJECT

public:
    explicit LoadMoreButton(QWidget *parent = nullptr);
};
