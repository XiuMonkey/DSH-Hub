#pragma once

// ------------------------------------------------------------------
// SpinnerWidget.h
// ------------------------------------------------------------------
// 现代化圆形加载旋转条，用于初始化/加载中的视觉反馈。
// ------------------------------------------------------------------

#include <QWidget>

class QTimer;

class SpinnerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SpinnerWidget(QWidget *parent = nullptr);

    void start();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QTimer *m_timer = nullptr;
    int m_angle = 0;
};
