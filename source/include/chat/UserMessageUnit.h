#pragma once

// 用户消息单元：QTextBrowser 子类，宽度限制在 [MinWidth, MaxWidth]，高度按内容自适应。

#include <QString>
#include <QTextBrowser>

class QResizeEvent;

class UserMessageUnit : public QTextBrowser
{
	Q_OBJECT

public:
	static constexpr int MaxWidth = 500;
	static constexpr int MinWidth = 80;

	explicit UserMessageUnit(QWidget* parent = nullptr);
	void setMessage(const QString& text);
	void updateHeightToContent();

protected:
	void resizeEvent(QResizeEvent* event) override;
};
