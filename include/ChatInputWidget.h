#pragma once

// ------------------------------------------------------------------
// ChatInputWidget.h
// ------------------------------------------------------------------
// 自动增高聊天输入框 + 附加发送按钮。
// 内部包含圆角输入胶囊、自动换行/增高、Enter 发送、Shift+Enter 换行。
// ------------------------------------------------------------------

#include <QString>
#include <QWidget>

class QPlainTextEdit;
class QPushButton;

class ChatInputWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ChatInputWidget(QWidget *parent = nullptr);

    // 获取当前输入内容（未 trim）
    QString text() const;

    // 清空输入框并复位高度
    void clear();

    // 设置占位提示文字
    void setPlaceholderText(const QString &placeholderText);

    // 让焦点回到输入框
    void focusInput();

    // 输入框控件（如需要直接操作 document / viewport 时可使用）
    QPlainTextEdit *editor() const;

    // 发送按钮（如需要调整样式/显隐时可使用）
    QPushButton *sendButton() const;

signals:
    // 点击发送按钮或按 Enter 时发出，携带当前输入框内容（未 trim）
    void sendRequested(const QString &text);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void handleSendClicked();

private:
    // 输入框随内容自动增高
    void adjustHeight();

    QPlainTextEdit *m_editor = nullptr;
    QPushButton *m_sendButton = nullptr;
};
