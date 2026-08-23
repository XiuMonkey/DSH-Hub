#pragma once

#include <QTextBrowser>

class QResizeEvent;

/**
 * 系统消息：无边框、斜体、浅灰色文字。
 * 用于显示连接状态、会话信息等日志类内容。
 */
class SystemMessageUnit : public QTextBrowser
{
public:
    static constexpr int DefaultWidth = 820;

    explicit SystemMessageUnit(QWidget *parent = nullptr);
    void setMessage(const QString &text);

    /** 根据内容自动调整高度。 */
    void updateHeightToContent();

protected:
    void resizeEvent(QResizeEvent *event) override;
};
