// ------------------------------------------------------------------
// AgentMessageUnit.cpp
// ------------------------------------------------------------------
// 可复用 Agent 消息展示控件的实现（气泡容器化 · 路线2 第2步）。
// 容器 = QWidget + QVBoxLayout 部件流：普通文本切 QTextBrowser(agentProse)，
// 代码围栏切 CodeBlockView 子单元，Thinking/Tool 用富文本锚点挂宿主 ProseView。
// ------------------------------------------------------------------

#include "AgentMessageUnit.h"
#include "CodeBlockView.h"
#include "CodeHighlighter.h"
#include "MarkdownPreprocess.h"
#include "ThemeManager.h"

#include <QAbstractTextDocumentLayout>
#include <QCryptographicHash>
#include <QFrame>
#include <QLabel>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextOption>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
	// 返回 widget（代码子单元的外层 QWidget，或代码块本体）里的 CodeBlockView；
	// 本控件内部唯一的文本编辑控件就是 CodeBlockView（objectName=codeBlockView）。
	QTextEdit* codeBlockEditIn(QWidget* widget)
	{
		if (!widget)
			return nullptr;
		if (auto* edit = qobject_cast<QTextEdit*>(widget))
			return edit;
		const QList<QTextEdit*> edits = widget->findChildren<QTextEdit*>();
		for (QTextEdit* edit : edits)
			return edit;
		return nullptr;
	}
}

AgentMessageUnit::AgentMessageUnit(QWidget* parent)
	: QWidget(parent)
{
	setObjectName(QStringLiteral("agentUnit")); // 外观规则见 resources/styles/chat.qss（#agentUnit / #agentBubble #agentUnit）
	setAttribute(Qt::WA_StyledBackground, true); // 让容器自己的 QSS 背景/圆角生效
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
	setFixedWidth(DefaultWidth);

	m_partsLayout = new QVBoxLayout(this);
	// 区域间距由各子部件自身控制（代码单元自带上下呼吸间距）；
	// 布局间距保持 0，避免“文本段 + 代码单元 + 文本段”之间产生额外叠加空白
	m_partsLayout->setContentsMargins(8, 2, 8, 2);
	m_partsLayout->setSpacing(0);
}

AgentMessageUnit::~AgentMessageUnit()
{
	// 子部件（ProseView / 代码子单元）都是 this 的孩子，随容器一起销毁即可。
}

QTextBrowser* AgentMessageUnit::makeProseView()
{
	QTextBrowser* view = createRichPart(QStringLiteral("agentProse"));
	m_proseViews.append(view);
	m_partsLayout->addWidget(view);
	return view;
}

QTextBrowser* AgentMessageUnit::createRichPart(const QString& objectName)
{
	auto* view = new QTextBrowser(this);
	view->setObjectName(objectName); // 透明/无边框外观见 chat.qss（#agentProse 等）

	view->setReadOnly(true);

	// 关闭内部滚动：高度交给 fitProseView()；水平滚动条按需，防止极端 HTML 撑宽
	view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	view->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	view->setMinimumWidth(0);
	view->setFrameShape(QFrame::NoFrame);
	view->setFrameShadow(QFrame::Plain);
	view->viewport()->setAutoFillBackground(false);
	view->document()->setDocumentMargin(0);

	// dsh:// 锚点由我们自己处理（展开/收起思考、工具），普通外链照常打开
	view->setOpenLinks(false);
	view->setOpenExternalLinks(true);
	connect(view, &QTextBrowser::anchorClicked,
		this, [this](const QUrl& url) { handleAnchorClicked(url); });

	// 内容变化后自适应高度（整批重建期间先抑制，重建结束统一拟合）
	connect(view->document(), &QTextDocument::contentsChanged,
		this, [this, view]() {
			if (!m_rebuilding)
				fitProseView(view);
		});

	return view;
}

QTextBrowser* AgentMessageUnit::proseHost()
{
	if (!m_proseViews.isEmpty()) {
		// 只有“末尾部件本身就是 ProseView”时才复用它，保证锚点/分隔始终追加在
		// 当前消息真实尾部之后（若末尾是代码块则另起新的空 ProseView）。
		QTextBrowser* tailProse = m_proseViews.last();
		QWidget* lastWidget = nullptr;
		if (m_partsLayout->count() > 0) {
			if (QLayoutItem* item = m_partsLayout->itemAt(m_partsLayout->count() - 1))
				lastWidget = item->widget();
		}
		if (static_cast<QWidget*>(tailProse) == lastWidget)
			return tailProse;
	}
	return makeProseView();
}

void AgentMessageUnit::fitProseView(QTextBrowser* view)
{
	int textWidth = view->viewport()->width();
	// 布局尚未生效时 viewport 宽度会很小/为 0：此时按容器固定内容宽度估算
	// （DefaultWidth - 16 = 720 减左右布局边距），否则会把文档按极窄宽度排版，
	// 算出一个离谱的高度
	if (textWidth < 300)
		textWidth = DefaultWidth - 16;

	view->document()->setTextWidth(textWidth);

	const qreal docHeight = view->document()->documentLayout()->documentSize().height();
	int height = static_cast<int>(docHeight + 0.9999);
	if (height <= 0)
		height = view->fontMetrics().height() + 2;
	view->setFixedHeight(height);
}

void AgentMessageUnit::refitParts()
{
	for (QTextBrowser* view : m_proseViews)
		fitProseView(view);
}

void AgentMessageUnit::updateHeightToContent()
{
	if (m_rebuilding)
		return;
	refitParts();

	// 子部件刚加入布局时几何可能还没生效（宽度仍很小），立即拟合会按错误宽度
	// 算出过高的固定高度；延后到事件循环里布局真正跑完后，再按真实宽度重算一次。
	QTimer::singleShot(0, this, [this]() {
		if (!m_rebuilding)
			refitParts();
	});
}

void AgentMessageUnit::resizeEvent(QResizeEvent* event)
{
	QWidget::resizeEvent(event);
	if (event->oldSize().width() != event->size().width())
		refitParts();
}

void AgentMessageUnit::clearParts()
{
	QLayoutItem* item = nullptr;
	while ((item = m_partsLayout->takeAt(0)) != nullptr) {
		if (QWidget* widget = item->widget()) {
			widget->hide();          // 先移出可视区
			widget->deleteLater();   // 延迟删除：可能正处在自身 anchorClicked 信号处理中
		}
		delete item;
	}
	m_proseViews.clear();
}

void AgentMessageUnit::insertMarkdownWithCodeShadow(const QString& markdown)
{
	// 统一换行符，避免 Windows \r\n 影响解析
	QString text = markdown;
	text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

	// 匹配 ```语言\n代码\n```
	QRegularExpression fenceRe(
		QStringLiteral("```([\\w+#-]*)\\s*\\n([\\s\\S]*?)```")
	);

	int pos = 0;
	QRegularExpressionMatchIterator it = fenceRe.globalMatch(text);

	while (it.hasNext()) {
		QRegularExpressionMatch match = it.next();

		// 代码块前面的普通 Markdown：一段普通文本 = 一个新 ProseView
		const QString before = text.mid(pos, match.capturedStart() - pos);
		if (!before.trimmed().isEmpty())
			appendProseRegion(before);

		// 代码围栏 → 独立代码子单元（语言标签 + CodeBlockView）
		addCodeBlockUnit(match.captured(1), match.captured(2));

		pos = match.capturedEnd();
	}

	// 最后剩余部分
	const QString after = text.mid(pos);
	if (!after.trimmed().isEmpty()) {
		// 流式输出时可能遇到还没闭合的 ``` 代码块，先按代码块渲染，等闭合后下次会正常解析
		QRegularExpression unclosedFenceRe(
			QStringLiteral("```([\\w+#-]*)\\s*\\n([\\s\\S]*)$")
		);
		QRegularExpressionMatch unclosedMatch = unclosedFenceRe.match(after);

		if (unclosedMatch.hasMatch()) {
			const QString before = after.left(unclosedMatch.capturedStart());
			if (!before.trimmed().isEmpty())
				appendProseRegion(before);
			addCodeBlockUnit(unclosedMatch.captured(1), unclosedMatch.captured(2));
		}
		else {
			appendProseRegion(after);
		}
	}
}

void AgentMessageUnit::appendMarkdownWithCodeShadow(const QString& markdown)
{
	Segment segment;
	segment.type = Segment::Markdown;
	segment.text = markdown;
	m_segments.append(segment);

	// 只渲染新增的这一段，而不是每次清空后全量重建，避免连续追加多段内容时
	// 变成 O(n^2) 的重复渲染；整段插入期间先抑制逐段高度重算，最后统一更新一次。
	m_rebuilding = true;
	insertMarkdownWithCodeShadow(markdown);
	m_rebuilding = false;
	updateHeightToContent();
}

void AgentMessageUnit::appendProseRegion(const QString& markdown)
{
	// 修剪首尾空白：markdown 段落边缘的换行会在转换后形成多余空段，
	// 这是“文本段之间大片空白”的主要来源之一
	QString text = markdown.trimmed();
	if (text.isEmpty())
		return;

	QTextBrowser* view = makeProseView();

	// 保留现状的渲染风格：临时 QTextDocument.setMarkdown → toHtml → insertHtml，
	// 并恢复行内代码占位符
	QStringList codes;
	const QString marked = replaceInlineCodeWithPlaceholders(text, codes);
	QTextDocument doc;
	doc.setMarkdown(prepareMarkdownForQt(marked));

	QTextCursor cursor = view->textCursor();
	cursor.movePosition(QTextCursor::End);
	cursor.insertHtml(restoreInlineCodeHtml(doc.toHtml(), codes));
	view->setTextCursor(cursor);
}

void AgentMessageUnit::addCodeBlockUnit(const QString& language, const QString& code)
{
	// 子单元 = 一个小 QVBoxLayout：[可选语言行(QLabel) + CodeBlockView]，
	// 整体作为一个布局子部件插入 m_partsLayout，保证与普通文本严格按顺序排布。
	auto* unit = new QWidget(this);
	auto* unitLayout = new QVBoxLayout(unit);
	// 代码单元上下各留少量呼吸间距（原 HTML 卡 margin 8 缩到 6，
	// 且布局间距已为 0，不会与段落空白叠加）
	unitLayout->setContentsMargins(0, 6, 0, 6);
	unitLayout->setSpacing(0);

	auto* langLabel = new QLabel(
		language.isEmpty() ? QStringLiteral("code") : language, unit);
	langLabel->setObjectName(QStringLiteral("codeBlockLang")); // 次要色外观见 chat.qss

	auto* codeView = new CodeBlockView(unit);
	codeView->setMinimumWidth(0); // 超长代码行交给横向滚动条，不撑宽容器
	// 语法高亮（灰色底由 chat.qss #codeBlockView 提供），无高亮规则时也能正常显示原文
	codeView->setCodeHtml(CodeHighlighter::instance().highlight(language, code));

	unitLayout->addWidget(langLabel);
	unitLayout->addWidget(codeView);
	m_partsLayout->addWidget(unit);
}

void AgentMessageUnit::insertHtml(const QString& html)
{
	QTextBrowser* host = proseHost();
	QTextCursor cursor = host->textCursor();
	cursor.movePosition(QTextCursor::End);
	cursor.insertHtml(html);
	host->setTextCursor(cursor);
}

void AgentMessageUnit::appendThinking(const QString& thinking)
{
	const int index = m_thinkingBlocks.size();
	m_thinkingBlocks.append({ thinking, m_expandedThinkingIndices.contains(index) });

	Segment segment;
	segment.type = Segment::Thinking;
	segment.thinkingIndex = index;
	m_segments.append(segment);

	// 只渲染新增的思考块，避免把已有回复全部重绘一遍。
	insertThinking(index);
	updateHeightToContent();
}

void AgentMessageUnit::appendToolCall(const QString& name, const QString& argumentsHtml)
{
	const int index = m_toolBlocks.size();
	const QString title = QStringLiteral("工具调用：%1").arg(name);
	m_toolBlocks.append({ title, argumentsHtml, m_expandedToolIndices.contains(index) });

	Segment segment;
	segment.type = Segment::Tool;
	segment.toolIndex = index;
	m_segments.append(segment);

	insertTool(index);
	updateHeightToContent();
}

void AgentMessageUnit::appendToolResult(const QString& resultHtml)
{
	const int index = m_toolBlocks.size();
	m_toolBlocks.append({ QStringLiteral("工具结果"), resultHtml, m_expandedToolIndices.contains(index) });

	Segment segment;
	segment.type = Segment::Tool;
	segment.toolIndex = index;
	m_segments.append(segment);

	insertTool(index);
	updateHeightToContent();
}

void AgentMessageUnit::appendSeparator()
{
	Segment segment;
	segment.type = Segment::Html;
	segment.text = QStringLiteral("<br>");
	m_segments.append(segment);

	insertHtml(segment.text);
	updateHeightToContent();
}

void AgentMessageUnit::resetContent()
{
	m_thinkingBlocks.clear();
	m_toolBlocks.clear();
	m_segments.clear();
	clearParts(); // 清空当前部件流，再增量渲染新的内容
}

void AgentMessageUnit::appendStreamChunk(StreamSegment::Type type, const QString& content,
	const QString& toolName)
{
	const bool mergeable = type != StreamSegment::ToolCall && type != StreamSegment::ToolResult;
	if (!m_streamSegments.isEmpty() && m_streamSegments.last().type == type && mergeable) {
		m_streamSegments.last().content += content;
	}
	else {
		StreamSegment segment;
		segment.type = type;
		segment.content = content;
		segment.toolName = toolName;
		m_streamSegments.append(segment);
	}
}

void AgentMessageUnit::flushStream()
{
	if (m_streamSegments.isEmpty())
		return;

	// 指纹去重：DSHHub 可能对同一批 chunk 触发多次 flush（追加即刷 + 定时器），
	// 内容没变时跳过整段重建，避免流式期间每帧都清空/重放整条消息。
	const QString fingerprint = streamFingerprint();
	if (fingerprint == m_lastFlushedFingerprint && hasContent())
		return;
	m_lastFlushedFingerprint = fingerprint;

	resetContent();

	// 按原始顺序渲染：思考 → 工具调用 → 回复 → 再思考 → ...
	for (const StreamSegment& segment : m_streamSegments) {
		switch (segment.type) {
		case StreamSegment::Thinking:
			appendThinking(segment.content);
			break;
		case StreamSegment::Reply:
			appendMarkdownWithCodeShadow(segment.content);
			break;
		case StreamSegment::ToolCall:
			appendToolCall(segment.toolName.isEmpty() ? QStringLiteral("工具") : segment.toolName,
				segment.content);
			break;
		case StreamSegment::ToolResult:
			appendToolResult(segment.content);
			break;
		}
	}
}

QString AgentMessageUnit::streamFingerprint() const
{
	QCryptographicHash hash(QCryptographicHash::Md5);
	for (const StreamSegment& segment : m_streamSegments) {
		hash.addData(QByteArray(1, static_cast<char>(segment.type)));
		hash.addData(segment.content.toUtf8());
		hash.addData(segment.toolName.toUtf8());
	}
	return QString::fromLatin1(hash.result().toHex());
}

void AgentMessageUnit::clearStreamSegments()
{
	m_streamSegments.clear();
	m_lastFlushedFingerprint.clear();
}

void AgentMessageUnit::rebuild()
{
	m_rebuilding = true;
	clearParts();

	for (const Segment& segment : m_segments) {
		switch (segment.type) {
		case Segment::Thinking:
			insertThinking(segment.thinkingIndex);
			break;
		case Segment::Tool:
			insertTool(segment.toolIndex);
			break;
		case Segment::Markdown:
			insertMarkdownWithCodeShadow(segment.text);
			break;
		case Segment::Html:
			insertHtml(segment.text);
			break;
		}
	}

	m_rebuilding = false;
	updateHeightToContent();
}

void AgentMessageUnit::insertThinking(int index)
{
	if (index < 0 || index >= m_thinkingBlocks.size())
		return;
	addThinkingCard(index);
}

void AgentMessageUnit::addThinkingCard(int index)
{
	ThinkingBlock& block = m_thinkingBlocks[index];
	const QString arrow = block.expanded ? QStringLiteral("▼") : QStringLiteral("▶");

	// 卡片 = 可点击标题（QLabel 富文本锚点）+ 展开时显示正文富文本
	auto* card = new QWidget(this);
	block.card = card;
	auto* layout = new QVBoxLayout(card);
	layout->setContentsMargins(0, 6, 0, 6);
	layout->setSpacing(4);

	auto* header = new QLabel(card);
	header->setObjectName(QStringLiteral("agentThinkHeader"));
	header->setTextFormat(Qt::RichText);
	header->setWordWrap(true);
	header->setTextInteractionFlags(Qt::TextBrowserInteraction);
	connect(header, &QLabel::linkActivated, this,
		[this](const QString& link) { handleAnchorClicked(QUrl(link)); });

	const QString anchor = QStringLiteral(
		"<a href=\"dsh://thinking/%1\" style=\"color:") + Theme::textSecondary()
		+ QStringLiteral("; text-decoration:none;\">%2 思考：%3</a>");
	header->setText(anchor.arg(index).arg(arrow, thinkingPreview(block.content).toHtmlEscaped()));
	layout->addWidget(header);

	if (block.expanded) {
		QTextBrowser* body = createRichPart(QStringLiteral("agentThinkBody"));
		block.body = body;
		const QString html = QStringLiteral(
			"<p style='color:") + Theme::textSecondary() + QStringLiteral(";'><i>%1</i></p>");
		body->setHtml(html.arg(block.content.toHtmlEscaped()));
		m_proseViews.append(body);
		layout->addWidget(body);
	}

	m_partsLayout->addWidget(card);
}

void AgentMessageUnit::updateThinkingCard(int index)
{
	ThinkingBlock& block = m_thinkingBlocks[index];
	if (!block.card)
		return; // 卡片还没建（异常路径），回到整条重建

	QLabel* header = block.card->findChild<QLabel*>(QStringLiteral("agentThinkHeader"));
	const QString arrow = block.expanded ? QStringLiteral("▼") : QStringLiteral("▶");
	if (header) {
		const QString anchor = QStringLiteral(
			"<a href=\"dsh://thinking/%1\" style=\"color:") + Theme::textSecondary()
			+ QStringLiteral("; text-decoration:none;\">%2 思考：%3</a>");
		header->setText(anchor.arg(index).arg(arrow, thinkingPreview(block.content).toHtmlEscaped()));
	}

	QVBoxLayout* cardLayout = qobject_cast<QVBoxLayout*>(block.card->layout());
	if (block.expanded && !block.body) {
		QTextBrowser* body = createRichPart(QStringLiteral("agentThinkBody"));
		block.body = body;
		const QString html = QStringLiteral(
			"<p style='color:") + Theme::textSecondary() + QStringLiteral(";'><i>%1</i></p>");
		body->setHtml(html.arg(block.content.toHtmlEscaped()));
		m_proseViews.append(body);
		if (cardLayout)
			cardLayout->addWidget(body);
	}
	else if (!block.expanded && block.body) {
		if (cardLayout)
			cardLayout->removeWidget(block.body);
		block.body->hide();
		block.body->deleteLater();
		m_proseViews.removeOne(block.body);
		block.body = nullptr;
	}

	updateHeightToContent();
}

void AgentMessageUnit::handleAnchorClicked(const QUrl& url)
{
	if (url.scheme() != QStringLiteral("dsh"))
		return;

	if (url.host() == QStringLiteral("thinking")) {
		bool ok = false;
		const int index = url.path().remove(0, 1).toInt(&ok);
		if (ok)
			toggleThinking(index);
	}
	else if (url.host() == QStringLiteral("tool")) {
		bool ok = false;
		const int index = url.path().remove(0, 1).toInt(&ok);
		if (ok)
			toggleTool(index);
	}
}

void AgentMessageUnit::toggleThinking(int index)
{
	if (index < 0 || index >= m_thinkingBlocks.size())
		return;

	ThinkingBlock& block = m_thinkingBlocks[index];
	block.expanded = !block.expanded;
	if (block.expanded)
		m_expandedThinkingIndices.insert(index);
	else
		m_expandedThinkingIndices.remove(index);

	// 就地展开/收起卡片（不整条重建，避免跳动/抖动）；异常时兜底整条重建
	if (block.card)
		updateThinkingCard(index);
	else
		rebuild();
}

void AgentMessageUnit::insertTool(int index)
{
	if (index < 0 || index >= m_toolBlocks.size())
		return;
	addToolCard(index);
}

void AgentMessageUnit::addToolCard(int index)
{
	ToolBlock& block = m_toolBlocks[index];
	const QString arrow = block.expanded ? QStringLiteral("▼") : QStringLiteral("▶");

	// 卡片 = 可点击标题 + 展开时显示工具参数/结果富文本
	auto* card = new QWidget(this);
	block.card = card;
	auto* layout = new QVBoxLayout(card);
	layout->setContentsMargins(0, 6, 0, 6);
	layout->setSpacing(4);

	auto* header = new QLabel(card);
	header->setObjectName(QStringLiteral("agentToolHeader"));
	header->setTextFormat(Qt::RichText);
	header->setWordWrap(true);
	header->setTextInteractionFlags(Qt::TextBrowserInteraction);
	connect(header, &QLabel::linkActivated, this,
		[this](const QString& link) { handleAnchorClicked(QUrl(link)); });

	const QString anchor = QStringLiteral(
		"<a href=\"dsh://tool/%1\" style=\"color:") + Theme::accent()
		+ QStringLiteral("; text-decoration:none;\">%2 %3</a>");
	header->setText(anchor.arg(index).arg(arrow, block.title.toHtmlEscaped()));
	layout->addWidget(header);

	if (block.expanded) {
		QTextBrowser* body = createRichPart(QStringLiteral("agentToolBody"));
		block.body = body;
		body->setHtml(block.content);
		m_proseViews.append(body);
		layout->addWidget(body);
	}

	m_partsLayout->addWidget(card);
}

void AgentMessageUnit::updateToolCard(int index)
{
	ToolBlock& block = m_toolBlocks[index];
	if (!block.card)
		return; // 卡片还没建（异常路径），回到整条重建

	QLabel* header = block.card->findChild<QLabel*>(QStringLiteral("agentToolHeader"));
	const QString arrow = block.expanded ? QStringLiteral("▼") : QStringLiteral("▶");
	if (header) {
		const QString anchor = QStringLiteral(
			"<a href=\"dsh://tool/%1\" style=\"color:") + Theme::accent()
			+ QStringLiteral("; text-decoration:none;\">%2 %3</a>");
		header->setText(anchor.arg(index).arg(arrow, block.title.toHtmlEscaped()));
	}

	QVBoxLayout* cardLayout = qobject_cast<QVBoxLayout*>(block.card->layout());
	if (block.expanded && !block.body) {
		QTextBrowser* body = createRichPart(QStringLiteral("agentToolBody"));
		block.body = body;
		body->setHtml(block.content);
		m_proseViews.append(body);
		if (cardLayout)
			cardLayout->addWidget(body);
	}
	else if (!block.expanded && block.body) {
		if (cardLayout)
			cardLayout->removeWidget(block.body);
		block.body->hide();
		block.body->deleteLater();
		m_proseViews.removeOne(block.body);
		block.body = nullptr;
	}

	updateHeightToContent();
}

void AgentMessageUnit::toggleTool(int index)
{
	if (index < 0 || index >= m_toolBlocks.size())
		return;

	ToolBlock& block = m_toolBlocks[index];
	block.expanded = !block.expanded;
	if (block.expanded)
		m_expandedToolIndices.insert(index);
	else
		m_expandedToolIndices.remove(index);

	if (block.card)
		updateToolCard(index);
	else
		rebuild();
}

QString AgentMessageUnit::thinkingPreview(const QString& content) const
{
	QString preview = content.simplified();
	if (preview.length() > 60)
		preview = preview.left(60) + QStringLiteral("...");
	return preview;
}

bool AgentMessageUnit::hasContent() const
{
	for (const QTextBrowser* view : m_proseViews) {
		if (view->document() && !view->document()->isEmpty())
			return true;
	}
	for (int i = 0; i < m_partsLayout->count(); ++i) {
		QLayoutItem* item = m_partsLayout->itemAt(i);
		if (item && codeBlockEditIn(item->widget()))
			return true;
	}
	return false;
}

QString AgentMessageUnit::textContent() const
{
	QString out;
	for (int i = 0; i < m_partsLayout->count(); ++i) {
		QLayoutItem* item = m_partsLayout->itemAt(i);
		QWidget* widget = item ? item->widget() : nullptr;
		if (!widget)
			continue;

		QString piece;
		if (auto* prose = qobject_cast<QTextBrowser*>(widget))
			piece = prose->toPlainText();
		else if (QTextEdit* code = codeBlockEditIn(widget))
			piece = code->toPlainText();
		else {
			// 思考/工具卡片：标题（QLabel，去掉 HTML 标签）与展开正文（QTextBrowser）
			for (QLabel* label : widget->findChildren<QLabel*>()) {
				QString text = label->text();
				text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
				text = text.trimmed();
				if (text.isEmpty())
					continue;
				if (!piece.isEmpty())
					piece += QLatin1Char('\n');
				piece += text;
			}
			for (QTextBrowser* sub : widget->findChildren<QTextBrowser*>()) {
				const QString text = sub->toPlainText().trimmed();
				if (text.isEmpty())
					continue;
				if (!piece.isEmpty())
					piece += QLatin1Char('\n');
				piece += text;
			}
		}
		if (piece.isEmpty())
			continue;

		if (!out.isEmpty())
			out += QLatin1Char('\n');
		out += piece;
	}
	return out;
}

QString AgentMessageUnit::replaceInlineCodeWithPlaceholders(const QString& markdown, QStringList& codes) const
{
	QRegularExpression inlineRe(QStringLiteral("`([^`]+)`"));

	QString output;
	int pos = 0;
	int index = 0;
	QRegularExpressionMatchIterator it = inlineRe.globalMatch(markdown);

	while (it.hasNext()) {
		QRegularExpressionMatch match = it.next();

		output += markdown.mid(pos, match.capturedStart() - pos);
		codes << match.captured(1);
		output += QStringLiteral("@@INLINE_CODE_%1@@").arg(index);

		++index;
		pos = match.capturedEnd();
	}

	output += markdown.mid(pos);
	return output;
}

QString AgentMessageUnit::restoreInlineCodeHtml(const QString& html, const QStringList& codes) const
{
	QString result = html;

	for (int i = 0; i < codes.size(); ++i) {
		const QString token = QStringLiteral("@@INLINE_CODE_%1@@").arg(i);
		const QString span = QStringLiteral(
			"<span style='"
			"background:") + Theme::inputBg() + QStringLiteral(
				";border:1px solid ") + Theme::border() + QStringLiteral(
					";box-shadow:0 1px 2px ") + Theme::color(QStringLiteral("shadowSubtle")) + QStringLiteral(";"
						"border-radius:4px;"
						"padding:1px 4px;"
						"font-family:Consolas,Menlo,monospace;"
						"font-size:0.9em;"
						"color:") + Theme::textPrimary() + QStringLiteral(
							";'>%1</span>"
						).arg(codes.at(i).toHtmlEscaped());
		result.replace(token, span);
	}

	return result;
}
