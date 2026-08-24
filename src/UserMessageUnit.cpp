#include "UserMessageUnit.h"

#include <QFrame>
#include <QTextOption>
#include <QAbstractTextDocumentLayout>
#include <QSizePolicy>
#include <QTextDocument>
#include <QFontMetrics>
#include <QtGlobal>
#include <QResizeEvent>

UserMessageUnit::UserMessageUnit(QWidget *parent)
    : QTextBrowser(parent)
{
    // 只读，不可编辑
    setReadOnly(true);

    // 去掉默认边框，使用圆角浅蓝色背景
    setFrameShape(QFrame::NoFrame);
    setFrameShadow(QFrame::Plain);

    setStyleSheet(QStringLiteral(
        "UserMessageUnit {"
        "  background-color: #E0F2FE;"
        "  border-radius: 12px;"
        "  color: #0C4A6E;"
        "}"
    ));

    // 使用 viewport margin 提供真正的内边距，而不是依赖 QSS padding
    setViewportMargins(8, 8, 8, 8);

    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

    // 用户气泡宽度由内容决定，最长不超过 MaxWidth
    setMaximumWidth(MaxWidth);
    setFixedWidth(MinWidth);

    // 高度由内容决定，不在控件内部显示滚动条
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    document()->setDocumentMargin(0);

    // 内容变化后自动调整高度
    connect(document(), &QTextDocument::contentsChanged,
            this, &UserMessageUnit::updateHeightToContent);
}

void UserMessageUnit::setMessage(const QString &text)
{
    // 先根据文本实际宽度算出气泡宽度
    QFontMetrics fm(font());
    int maxLineWidth = 0;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        maxLineWidth = qMax(maxLineWidth, fm.horizontalAdvance(line));
    }

    // 左右 padding + 边框占位，粗略估算
    const int padding = 16;
    const int idealWidth = maxLineWidth + padding + frameWidth() * 2;
    const int bubbleWidth = qBound(MinWidth, idealWidth, MaxWidth);

    // 先确定宽度，再填入文本，这样高度只会按最终宽度计算一次
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

    // documentSize() 已包含 QTextDocument 自身的边距，
    // 这里只需要再补上 QSS 里上下各 8px 的 padding 和边框高度。
    const qreal docHeight = document()->documentLayout()->documentSize().height();
    const int verticalPadding = 16; // QSS padding: 8px top + 8px bottom
    const int frame = frameWidth() * 2;

    setFixedHeight(static_cast<int>(docHeight) + verticalPadding + frame);
}

void UserMessageUnit::resizeEvent(QResizeEvent *event)
{
    QTextBrowser::resizeEvent(event);

    // 宽度变化后，文本换行位置会变，需要重新计算刚好包裹文本的高度
    if (event->oldSize().width() != event->size().width())
        updateHeightToContent();
}
