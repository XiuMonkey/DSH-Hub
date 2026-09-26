#pragma once

// 系统消息单元：无边框、斜体、浅灰文字的日志类内容（连接状态、会话信息）。

#include <QTextBrowser>

class QResizeEvent;

class SystemMessageUnit : public QTextBrowser
{
public:
	static constexpr int DefaultWidth = 820;

	explicit SystemMessageUnit(QWidget* parent = nullptr);
	void setMessage(const QString& text);
	void updateHeightToContent();

protected:
	void resizeEvent(QResizeEvent* event) override;
};
