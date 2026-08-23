#pragma once

// ------------------------------------------------------------------
// SessionButton.h
// ------------------------------------------------------------------
// 会话列表中的单个会话按钮。
// 继承 QPushButton，显示一行会话标题，超长时使用省略号截断。
// ------------------------------------------------------------------

#include <QPushButton>
#include <QString>

class QResizeEvent;

class SessionButton : public QPushButton
{
    Q_OBJECT

public:
    explicit SessionButton(const QString &sessionId,
                           const QString &title,
                           QWidget *parent = nullptr);

    QString sessionId() const;

    // 原始完整标题
    QString fullTitle() const;

    // 更新显示标题（内部会重新计算省略号文本）
    void setSessionTitle(const QString &title);

    // 设置当前选中状态
    void setSelected(bool selected);

signals:
    // 点击该会话按钮时发出
    void sessionClicked(const QString &sessionId);

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void handleClicked();

private:
    void updateElidedText();

    QString m_sessionId;
    QString m_fullTitle;
};
