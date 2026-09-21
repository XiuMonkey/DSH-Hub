#include "chat/CodeBlockView.h"

#include "common/appearance/ThemeManager.h"

#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QTextDocument>
#include <QTextOption>

CodeBlockView::CodeBlockView(QWidget* parent)
	: QTextEdit(parent)
{
	setObjectName(QStringLiteral("codeBlockView")); // 外观见 resources/styles/chat.qss（灰色底 #codeBlockView）

	// 滚动条是在 QTextEdit 基类构造里建好的，那时 objectName 还没设，
	// QStyleSheetStyle 会把"匹配不到 #codeBlockView QScrollBar"缓存下来，
	// 之后 setObjectName 不会触发重新匹配 —— 滚动条会一直是原生（老式）样式。
	// 这里设完 objectName 后强制重新解析一次。
	ThemeManager::instance().repolishScrollArea(this);

	setReadOnly(true);
	setUndoRedoEnabled(false);
	setFrameShape(QFrame::NoFrame);

	// 等宽字体（代码内容）
	QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	fixed.setStyleHint(QFont::Monospace);
	fixed.setPointSize(9);
	setFont(fixed);

	// 不换行：超长行交给横向滚动条；纵向滚动条按需
	setWordWrapMode(QTextOption::NoWrap);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

	// 内容边距（QSS padding 对滚动区文本控件不生效，用 viewport margin）
	setViewportMargins(6, 6, 6, 6);

	connect(document(), &QTextDocument::contentsChanged,
		this, &CodeBlockView::updateHeightToContent);
}

void CodeBlockView::setCode(const QString& code)
{
	setPlainText(code);
	updateHeightToContent();
}

void CodeBlockView::setCodeHtml(const QString& html)
{
	// 用 <pre> 包装：保留缩进与换行（Qt 富文本只在 <pre> 内保留空白），
	// 字体与控件自身的等宽设置一致
	setHtml(QStringLiteral("<pre style='margin:0;font-family:Consolas,Menlo,monospace;'>")
		+ html + QStringLiteral("</pre>"));
	updateHeightToContent();
}

void CodeBlockView::setMaxHeight(int maxHeight)
{
	m_maxHeight = qMax(16, maxHeight);
	updateHeightToContent();
}

void CodeBlockView::updateHeightToContent()
{
	// 不依赖惰性的 documentLayout()->documentSize()（见历史 bug：会算成单行），
	// 按“行数 × 行高”计算：NoWrap + 源码换行结构下，每行源码占一行。
	const QString text = toPlainText();
	const int lines = text.isEmpty() ? 1 : (text.count(QLatin1Char('\n')) + 1);
	const int lineHeight = fontMetrics().lineSpacing();
	int height = lines * lineHeight + 20; // 20 = viewport 上下边距(6+6) + 文档 margin(约8)
	if (height <= 0)
		height = lineHeight + 20;

	// 内容超高时封顶并允许纵向滚动；否则恰好包裹内容
	if (height > m_maxHeight) {
		setFixedHeight(m_maxHeight);
		setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	}
	else {
		setFixedHeight(height);
		setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	}
}