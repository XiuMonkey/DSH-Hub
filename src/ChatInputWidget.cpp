#include "ChatInputWidget.h"
#include "ThemeManager.h"

#include <QAbstractTextDocumentLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSize>
#include <QSizePolicy>
#include <QTextOption>
#include <QTimer>

ChatInputWidget::ChatInputWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("inputCapsule"));
    // 让 QWidget 子类真正绘制样式表里的背景和边框
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("#inputCapsule {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border: 1px solid ") + QStringLiteral("#D0D7DE") + QStringLiteral(";") + QStringLiteral("  border-radius: 22px;") + QStringLiteral("}") + QStringLiteral("QPlainTextEdit {") + QStringLiteral("  border: none;") + QStringLiteral("  background: transparent;") + QStringLiteral("  padding: 8px 12px;") + QStringLiteral("  font-size: 14px;") + QStringLiteral("}") + QStringLiteral("QPushButton#sendButton {") + QStringLiteral("  border: none;") + QStringLiteral("  background: transparent;") + QStringLiteral("  border-radius: 16px;") + QStringLiteral("  min-width: 32px;") + QStringLiteral("  max-width: 32px;") + QStringLiteral("  min-height: 32px;") + QStringLiteral("  max-height: 32px;") + QStringLiteral("  padding: 0;") + QStringLiteral("}") + QStringLiteral("QPushButton#sendButton:hover {") + QStringLiteral("  background: ") + QStringLiteral("#E0E7FF") + QStringLiteral(";") + QStringLiteral("  border-radius: 16px;") + QStringLiteral("}") + QStringLiteral("QPushButton#sendButton:pressed {") + QStringLiteral("  background: ") + QStringLiteral("#C7D2FE") + QStringLiteral(";") + QStringLiteral("  border-radius: 16px;") + QStringLiteral("}"));

    m_editor = new QPlainTextEdit(this);
    m_editor->setFrameShape(QFrame::NoFrame);
    // 隐藏输入框滚动条，但保留鼠标滚轮滚动能力
    m_editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 必须显式开启 WidgetWidth 换行，再配合 wordWrapMode 控制断行方式。
    m_editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_editor->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_editor->setFixedHeight(36);
    m_editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_editor->setPlaceholderText(QStringLiteral("输入消息，Enter 发送，Shift+Enter 换行"));

    m_sendButton = new QPushButton(this);
    m_sendButton->setObjectName(QStringLiteral("sendButton"));
    // 使用内置到 exe 的 EnterBtn.png 作为发送按钮图标
    m_sendButton->setIcon(QIcon(QStringLiteral(":/DSHHub/EnterBtn.png")));
    m_sendButton->setIconSize(QSize(24, 24));
    m_sendButton->setCursor(Qt::PointingHandCursor);
    m_sendButton->setToolTip(QStringLiteral("发送"));
    m_sendButton->setFixedSize(32, 32);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addWidget(m_editor, 1);
    layout->addWidget(m_sendButton);

    m_editor->installEventFilter(this);

    connect(m_sendButton, &QPushButton::clicked, this, &ChatInputWidget::handleSendClicked);
    connect(m_editor->document(), &QTextDocument::contentsChanged, this, [this]() {
        // 放到事件循环里再算，确保 QPlainTextEdit 已经用当前 viewport 宽度完成内部布局
        QTimer::singleShot(0, this, [this]() { adjustHeight(); });
    });

    QTimer::singleShot(0, this, [this]() { adjustHeight(); });
}

QString ChatInputWidget::text() const
{
    return m_editor ? m_editor->toPlainText() : QString();
}

void ChatInputWidget::clear()
{
    if (!m_editor)
        return;

    m_editor->clear();
    adjustHeight();
}

void ChatInputWidget::handleSendClicked()
{
    if (m_editor)
        emit sendRequested(m_editor->toPlainText());
}

bool ChatInputWidget::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_editor && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)
            && !(keyEvent->modifiers() & Qt::ShiftModifier)) {
            emit sendRequested(m_editor->toPlainText());
            return true;
        }
    } else if (obj == m_editor && event->type() == QEvent::Resize) {
        // 输入框宽度变化后，换行位置/高度需要重新计算
        QTimer::singleShot(0, this, [this]() { adjustHeight(); });
    }

    return QWidget::eventFilter(obj, event);
}

void ChatInputWidget::adjustHeight()
{
    if (!m_editor)
        return;

    // QPlainTextEdit 的换行由 lineWrapMode + wordWrapMode 共同控制。
    if (m_editor->lineWrapMode() != QPlainTextEdit::WidgetWidth)
        m_editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);

    int textWidth = m_editor->viewport()->width();
    if (textWidth <= 0)
        textWidth = m_editor->width() - 4;
    if (textWidth <= 0)
        textWidth = 1;

    // 让文档按当前可视宽度排版。
    m_editor->document()->setTextWidth(textWidth);

    // QPlainTextEdit 实际使用内部的 QPlainTextDocumentLayout，只有它知道自己的私有行宽。
    // 这里临时切换一次 lineWrapMode，强制它按当前 viewport 宽度重新排版后再取高度。
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);

    const qreal docHeight = m_editor->document()->documentLayout()->documentSize().height();
    // QPlainTextEdit 的 QPlainTextDocumentLayout 高度单位是“行数”，不是像素。
    const qreal lineHeight = m_editor->fontMetrics().lineSpacing();
    // +16 对应 QSS 中 QPlainTextEdit 的上下 padding（8px + 8px）
    const int contentHeight = static_cast<int>(docHeight * lineHeight + 0.5) + 16;
    const int newHeight = qBound(36, contentHeight, 120);

    if (m_editor->height() != newHeight) {
        m_editor->setFixedHeight(newHeight);
        m_editor->updateGeometry();

        // 通知父级布局重新计算，避免 setFixedHeight 后外层容器没有立即跟随变化
        if (QWidget *parent = m_editor->parentWidget()) {
            if (QLayout *parentLayout = parent->layout())
                parentLayout->activate();
            if (QWidget *grandParent = parent->parentWidget()) {
                grandParent->updateGeometry();
                if (QLayout *grandParentLayout = grandParent->layout())
                    grandParentLayout->activate();
            }
        }
    }

    // 如果内容完整可见，要把滚动位置拉回顶部。
    // 否则 QPlainTextEdit 之前为了跟随光标产生的滚动偏移会让第一行被顶出视野。
    if (contentHeight <= newHeight)
        m_editor->verticalScrollBar()->setValue(0);
}
