#include "chat/SystemMessageUnit.h"

#include <QAbstractTextDocumentLayout>
#include <QFrame>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QTextDocument>

SystemMessageUnit::SystemMessageUnit(QWidget* parent)
	: QTextBrowser(parent)
{
	setReadOnly(true);

	// 无边框、透明背景、斜体浅灰文字

	setObjectName(QStringLiteral("systemUnit")); // 外观规则见 resources/styles/chat.qss（#systemUnit）
	viewport()->setAutoFillBackground(false);
	setFrameShape(QFrame::NoFrame);
	setFrameShadow(QFrame::Plain);

	setFixedWidth(DefaultWidth);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
	setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	// 单行过长时给左右滚动条，避免内容被裁剪（正常文本仍按宽度换行）
	setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	document()->setDocumentMargin(0);

	connect(document(), &QTextDocument::contentsChanged,
		this, &SystemMessageUnit::updateHeightToContent);
}

void SystemMessageUnit::setMessage(const QString& text)
{
	setPlainText(text);
	updateHeightToContent();
}

void SystemMessageUnit::updateHeightToContent()
{
	int textWidth = viewport()->width();
	if (textWidth <= 0)
		textWidth = width() - frameWidth() * 2 - 8;

	document()->setTextWidth(textWidth);

	const qreal docHeight = document()->documentLayout()->documentSize().height();
	const int frame = frameWidth() * 2;

	setFixedHeight(static_cast<int>(docHeight) + frame);
}

void SystemMessageUnit::resizeEvent(QResizeEvent* event)
{
	QTextBrowser::resizeEvent(event);

	if (event->oldSize().width() != event->size().width())
		updateHeightToContent();
}