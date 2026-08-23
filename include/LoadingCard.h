#pragma once

// ------------------------------------------------------------------
// LoadingCard.h
// ------------------------------------------------------------------
// 居中显示的“正在加载会话”卡片。
// 自带“...”动画，由 DSHHub 负责放到对话区域并做整体垂直居中。
// ------------------------------------------------------------------

#include <QLabel>

class QTimer;

class LoadingCard : public QLabel
{
    Q_OBJECT

public:
    explicit LoadingCard(QWidget *parent = nullptr);

private:
    QTimer *m_timer = nullptr;
    int m_dots = 0;
};
