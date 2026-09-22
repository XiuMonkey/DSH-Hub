#pragma once

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

	/** 根据内容自动调整控件高度。 */
	void updateHeightToContent();

protected:
	void resizeEvent(QResizeEvent* event) override;
};