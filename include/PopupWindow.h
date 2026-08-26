#pragma once

// ------------------------------------------------------------------
// PopupWindow.h
// ------------------------------------------------------------------
// 无系统边框的现代弹出窗口：
//   - 无 Windows 原生边框
//   - 圆角
//   - 白色背景
//   - 灰色细边框
//   - 右上角关闭按钮
// ------------------------------------------------------------------

#include <QWidget>

class QCloseEvent;
class QLabel;
class QPushButton;
class QVBoxLayout;

class PopupWindow : public QWidget
{
    Q_OBJECT

public:
    explicit PopupWindow(QWidget *parent = nullptr);

    void setTitle(const QString &title);
    void setContent(QWidget *content);

signals:
    void closed();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QLabel *m_titleLabel = nullptr;
    QPushButton *m_closeButton = nullptr;
    QVBoxLayout *m_mainLayout = nullptr;
    QVBoxLayout *m_contentLayout = nullptr;
};
