#include "chat/UserMessageUnit.h"

#include <QFrame>
#include <QStringList>
#include <QTextOption>
#include <QAbstractTextDocumentLayout>
#include <QSizePolicy>
#include <QTextDocument>
#include <QFontMetrics>
#include <QtGlobal>
#include <QResizeEvent>

UserMessageUnit::UserMessageUnit(QWidget* parent)
	: QTextBrowser(parent)
{
	setReadOnly(true);
	setFrameShape(QFrame::NoFrame);
	setFrameShadow(QFrame::Plain);

	setObjectName(QStringLiteral("userUnit")); // 外观见 chat.qss #userUnit

	// 内边距靠 viewport margin，不靠 QSS padding
	setViewportMargins(8, 8, 8, 8);
	setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

	// 宽度随内容、上限 MaxWidth；单行过长给横向滚动条
	setMaximumWidth(MaxWidth);
	setFixedWidth(MinWidth);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
	setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	document()->setDocumentMargin(0);

	connect(document(), &QTextDocument::contentsChanged, this, &UserMessageUnit::updateHeightToContent);
}

void UserMessageUnit::setMessage(const QString& text)
{
	QFontMetrics fm(font());
	int maxLineWidth = 0;
	const QStringList lines = text.split(QLatin1Char('\n'));
	for (const QString& line : lines) {
		maxLineWidth = qMax(maxLineWidth, fm.horizontalAdvance(line));
	}

	const int padding = 16;
	const int idealWidth = maxLineWidth + padding + frameWidth() * 2;
	const int bubbleWidth = qBound(MinWidth, idealWidth, MaxWidth);

	// 先定宽再填文本，高度只算一次
	setFixedWidth(bubbleWidth);
	setPlainText(text);
	updateHeightToContent();
}

void UserMessageUnit::updateHeightToContent()
{
	int textWidth = viewport()->width();
	if (textWidth <= 0)
		textWidth = width() - frameWidth() * 2 - 8;

	document()->setTextWidth(textWidth);

	// documentSize() 已含文档边距，只补 QSS padding 与边框
	const qreal docHeight = document()->documentLayout()->documentSize().height();
	const int verticalPadding = 16;
	const int frame = frameWidth() * 2;

	setFixedHeight(static_cast<int>(docHeight) + verticalPadding + frame);
}

void UserMessageUnit::resizeEvent(QResizeEvent* event)
{
	QTextBrowser::resizeEvent(event);

	// 换行位置会变，重算包裹高度
	if (event->oldSize().width() != event->size().width())
		updateHeightToContent();
}
