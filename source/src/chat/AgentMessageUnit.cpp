// 可复用 Agent 消息展示控件：容器 = QWidget + QVBoxLayout 部件流 —— 普通文本切
// QTextBrowser(agentProse)，代码围栏切 CodeBlockView 子单元，Thinking/Tool 用富文本锚点。

#include "chat/AgentMessageUnit.h"
#include "ui/LayoutUtils.h"
#include "chat/CodeBlockView.h"
#include "common/util/CodeHighlighter.h"
#include "common/util/MarkdownPreprocess.h"
#include "common/appearance/ThemeManager.h"

#include <QAbstractTextDocumentLayout>
#include <QCryptographicHash>
#include <QElapsedTimer>
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
	bool renderTraceEnabled()
	{
		static const bool on = qEnvironmentVariableIsSet("DSH_HUB_RENDER_TRACE");
		return on;
	}

	void traceRender(const char* where, qint64 ms)
	{
		if (renderTraceEnabled())
			qInfo().noquote() << "[Render]" << where << ms << "ms";
	}

	bool layoutTraceEnabled()
	{
		static const bool on = qEnvironmentVariableIsSet("DSH_HUB_LAYOUT_TRACE");
		return on;
	}
}

namespace
{
	// 取 widget 里唯一的 QTextEdit（CodeBlockView，objectName=codeBlockView）
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

	// 思考卡锚点；颜色须现取不能缓存，主题切换靠重建窗口
	QString thinkingAnchorHtml(int index, const QString& arrow, const QString& preview)
	{
		return (QStringLiteral("<a href=\"dsh://thinking/%1\" style=\"color:")
			+ ThemeManager::instance().textSecondary()
			+ QStringLiteral("; text-decoration:none;\">") + qtTrId("chat_thought_header_fmt")
			+ QStringLiteral("</a>")).arg(index).arg(arrow, preview);
	}

	QString thinkingBodyHtml(const QString& content)
	{
		return (QStringLiteral("<p style='color:") + ThemeManager::instance().textSecondary()
			+ QStringLiteral(";'><i>%1</i></p>")).arg(content.toHtmlEscaped());
	}

	// 工具卡锚点；标题由调用方拼好并已转义，这里不再处理
	QString toolAnchorHtml(int index, const QString& arrow, const QString& title)
	{
		return (QStringLiteral("<a href=\"dsh://tool/%1\" style=\"color:")
			+ ThemeManager::instance().accent()
			+ QStringLiteral("; text-decoration:none;\">%2 %3</a>")).arg(index).arg(arrow, title);
	}
}

AgentMessageUnit::AgentMessageUnit(QWidget* parent)
	: QWidget(parent)
{
	setObjectName(QStringLiteral("agentUnit"));
	setAttribute(Qt::WA_StyledBackground, true);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
	setFixedWidth(DefaultWidth);

	// 间距由子部件自控，布局间距保持 0，避免叠加出额外空白
	m_partsLayout = new QVBoxLayout(this);
	m_partsLayout->setContentsMargins(8, 2, 8, 2);
	m_partsLayout->setSpacing(0);
}

AgentMessageUnit::~AgentMessageUnit()
{
	// 子部件随容器销毁
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
	view->setObjectName(objectName);

	// 滚动条在基类构造时就建好（那时还没 objectName），设完名字后必须重新 polish
	ThemeManager::instance().repolishScrollArea(view);

	view->setReadOnly(true);

	// 高度交给 fitProseView()，须关内部纵向滚动；横向按需防撑宽
	view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	view->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	view->setMinimumWidth(0);
	view->setFrameShape(QFrame::NoFrame);
	view->setFrameShadow(QFrame::Plain);
	view->viewport()->setAutoFillBackground(false);
	view->document()->setDocumentMargin(0);

	// dsh:// 锚点自行处理，普通外链照常打开；view 随内容重建，不进登记表
	view->setOpenLinks(false);
	view->setOpenExternalLinks(true);
	connect(view, &QTextBrowser::anchorClicked,
		this, [this](const QUrl& url) { handleAnchorClicked(url); });

	// 内容变化后自适应高度；整批重建期间抑制，结束后统一拟合
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
		// 只有末尾部件本身是 ProseView 才复用，否则锚点会追加到代码块之前
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
	// 新建视图布局前宽度仍是 Qt 默认 100px，必须先校正宽度再 setFixedHeight，
	// 否则按窄宽算出的高度会瞬间撑高气泡（流式抖动的来源）
	const QMargins margins = m_partsLayout->contentsMargins();
	const int contentWidth = qMax(1, width() - margins.left() - margins.right());
	if (view->width() != contentWidth)
		view->setFixedWidth(contentWidth);

	view->document()->setTextWidth(contentWidth);

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

	QElapsedTimer timer;
	timer.start();

	if (layoutTraceEnabled())
		debugTraceLayout(QStringLiteral("before"));

	refitParts();

	if (layoutTraceEnabled())
		debugTraceLayout(QStringLiteral("after-refit"));

	// 布局几何尚未生效时拟合会算出过高高度，延后到事件循环再按真实宽度重算
	QTimer::singleShot(0, this, [this]() {
		if (m_rebuilding)
			return;
		refitParts();
		if (layoutTraceEnabled())
			debugTraceLayout(QStringLiteral("after-settle"));
		});

	traceRender("updateHeightToContent", timer.elapsed());
}

void AgentMessageUnit::debugTraceLayout(const QString& stage) const
{
	QStringList parts;
	parts << QStringLiteral("stage=%1 unit=%2x%3 hint=%4x%5").arg(stage).arg(width()).arg(height())
		.arg(sizeHint().width()).arg(sizeHint().height());

	for (int i = 0; i < m_partsLayout->count(); ++i) {
		QLayoutItem* item = m_partsLayout->itemAt(i);
		QWidget* widget = item ? item->widget() : nullptr;
		if (!widget)
			continue;
		parts << QStringLiteral("[%1#%2 %3x%4 hintH=%5]")
			.arg(QString::fromLatin1(widget->metaObject()->className()), widget->objectName())
			.arg(widget->width()).arg(widget->height()).arg(widget->sizeHint().height());
	}

	for (QLabel* label : findChildren<QLabel*>()) {
		if (!label->wordWrap())
			continue;
		parts << QStringLiteral("{QLabel#%1 w=%2 h=%3 hintH=%4 hfw=%5}").arg(label->objectName())
			.arg(label->width()).arg(label->height()).arg(label->sizeHint().height())
			.arg(label->heightForWidth(label->width()));
	}

	qInfo().noquote() << QStringLiteral("[LayoutTrace]") << parts.join(QLatin1Char(' '));
}

void AgentMessageUnit::resizeEvent(QResizeEvent* event)
{
	QWidget::resizeEvent(event);
	if (event->oldSize().width() != event->size().width())
		refitParts();
}

void AgentMessageUnit::clearParts()
{
	// 不摘离控件树：此处可能正处在子控件的 anchorClicked 处理中
	LayoutUtils::clearLayout(m_partsLayout, LayoutUtils::ClearMode::HideThenDefer);
	m_proseViews.clear();
}

void AgentMessageUnit::insertMarkdownWithCodeShadow(const QString& markdown)
{
	QString text = markdown;
	text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

	QRegularExpression fenceRe(QStringLiteral("```([\\w+#-]*)\\s*\\n([\\s\\S]*?)```"));

	int pos = 0;
	QRegularExpressionMatchIterator it = fenceRe.globalMatch(text);

	while (it.hasNext()) {
		QRegularExpressionMatch match = it.next();

		const QString before = text.mid(pos, match.capturedStart() - pos);
		if (!before.trimmed().isEmpty())
			appendProseRegion(before);

		addCodeBlockUnit(match.captured(1), match.captured(2));

		pos = match.capturedEnd();
	}

	const QString after = text.mid(pos);
	if (!after.trimmed().isEmpty()) {
		// 流式输出可能遇到未闭合的 ``` 代码块，先按代码块渲染，闭合后重解析
		QRegularExpression unclosedFenceRe(QStringLiteral("```([\\w+#-]*)\\s*\\n([\\s\\S]*)$"));
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
	QElapsedTimer timer;
	timer.start();

	Segment segment;
	segment.type = Segment::Markdown;
	segment.text = markdown;
	m_segments.append(segment);

	// 只渲染新增段，避免连续追加变成 O(n^2)；插入期间抑制逐段高度重算
	m_rebuilding = true;
	insertMarkdownWithCodeShadow(markdown);
	m_rebuilding = false;
	if (!m_bulkFit)
		updateHeightToContent();

	traceRender("appendMarkdownWithCodeShadow", timer.elapsed());
}

void AgentMessageUnit::appendProseRegion(const QString& markdown)
{
	// 修剪首尾空白：段落边缘的换行会形成多余空段，是文本段间大空白的主因
	QString text = markdown.trimmed();
	if (text.isEmpty())
		return;

	QElapsedTimer timer;
	timer.start();

	QTextBrowser* view = makeProseView();

	QStringList codes;
	const QString marked = replaceInlineCodeWithPlaceholders(text, codes);
	QTextDocument doc;
	doc.setMarkdown(prepareMarkdownForQt(marked));

	QTextCursor cursor = view->textCursor();
	cursor.movePosition(QTextCursor::End);
	cursor.insertHtml(restoreInlineCodeHtml(doc.toHtml(), codes));
	view->setTextCursor(cursor);

	traceRender("appendProseRegion", timer.elapsed());
}

void AgentMessageUnit::addCodeBlockUnit(const QString& language, const QString& code)
{
	QElapsedTimer timer;
	timer.start();

	// 子单元 = [语言行(QLabel) + CodeBlockView]，作为布局子部件插入以保持顺序
	auto* unit = new QWidget(this);
	auto* unitLayout = new QVBoxLayout(unit);
	unitLayout->setContentsMargins(0, 6, 0, 6);
	unitLayout->setSpacing(0);

	auto* langLabel = new QLabel(language.isEmpty() ? QStringLiteral("code") : language, unit);
	langLabel->setObjectName(QStringLiteral("codeBlockLang"));

	auto* codeView = new CodeBlockView(unit);
	codeView->setMinimumWidth(0);
	codeView->setCodeHtml(CodeHighlighter::instance().highlight(language, code));

	unitLayout->addWidget(langLabel);
	unitLayout->addWidget(codeView);
	m_partsLayout->addWidget(unit);

	traceRender("addCodeBlockUnit", timer.elapsed());
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

	insertThinking(index);
	if (!m_bulkFit)
		updateHeightToContent();
}

void AgentMessageUnit::appendToolCall(const QString& name, const QString& argumentsHtml)
{
	const int index = m_toolBlocks.size();
	const QString title = qtTrId("chat_tool_call_fmt").arg(name);
	m_toolBlocks.append({ title, argumentsHtml, m_expandedToolIndices.contains(index) });

	Segment segment;
	segment.type = Segment::Tool;
	segment.toolIndex = index;
	m_segments.append(segment);

	insertTool(index);
	if (!m_bulkFit)
		updateHeightToContent();
}

void AgentMessageUnit::appendToolResult(const QString& resultHtml)
{
	const int index = m_toolBlocks.size();
	m_toolBlocks.append({ qtTrId("chat_tool_result_label"), resultHtml, m_expandedToolIndices.contains(index) });

	Segment segment;
	segment.type = Segment::Tool;
	segment.toolIndex = index;
	m_segments.append(segment);

	insertTool(index);
	if (!m_bulkFit)
		updateHeightToContent();
}

void AgentMessageUnit::appendSeparator()
{
	Segment segment;
	segment.type = Segment::Html;
	segment.text = QStringLiteral("<br>");
	m_segments.append(segment);

	insertHtml(segment.text);
	if (!m_bulkFit)
		updateHeightToContent();
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

	// 指纹去重：DSHHub 可能对同一批 chunk 多次 flush，内容没变就跳过
	const QString fingerprint = streamFingerprint();
	if (fingerprint == m_lastFlushedFingerprint && hasContent())
		return;
	m_lastFlushedFingerprint = fingerprint;

	const int count = m_streamSegments.size();

	// live 思考段不再是尾部时先封闭并计入已封存数，防止下方循环重复建卡
	if (m_liveThinkingSegment >= 0) {
		const bool stillTailThinking = (m_liveThinkingSegment == count - 1)
			&& m_streamSegments.last().type == StreamSegment::Thinking;
		if (!stillTailThinking)
			sealLiveThinking();
	}

	int i = 0;
	while (i < count) {
		const StreamSegment& seg = m_streamSegments.at(i);
		const bool isTail = (i == count - 1);

		// 尾部回复走 live 增量，只更新最后一段的尾部区域
		if (isTail && seg.type == StreamSegment::Reply) {
			if (m_liveIndex != i) {
				if (m_liveIndex >= 0)
					closeLiveReply();
				m_liveIndex = i;
				m_liveLayoutMark = m_partsLayout->count();
				m_liveSegmentEntry = -1;
				m_liveRenderedText.clear();
			}
			renderLiveReply(seg.content);
			return;
		}

		// 尾部思考也随 token 增长，像 Reply 一样 live 更新
		if (isTail && seg.type == StreamSegment::Thinking) {
			if (m_liveIndex >= 0)
				closeLiveReply();
			if (m_liveThinkingSegment != i) {
				m_liveThinkingSegment = i;
				m_liveThinkingBlock = -1;
			}
			updateLiveThinking(seg.content);
			return;
		}

		// 非尾部段：封闭上一轮 live 段，或对首次出现的段用 append* 一次性渲染
		if (i == m_liveIndex) {
			closeLiveReply();
			m_streamSealedCount = i + 1;
		}
		else if (i >= m_streamSealedCount) {
			switch (seg.type) {
			case StreamSegment::Thinking:
				appendThinking(seg.content);
				break;
			case StreamSegment::Reply:
				appendMarkdownWithCodeShadow(seg.content);
				break;
			case StreamSegment::ToolCall:
				appendToolCall(seg.toolName.isEmpty() ? qtTrId("chat_tool_label") : seg.toolName, seg.content);
				break;
			case StreamSegment::ToolResult:
				appendToolResult(seg.content);
				break;
			}
			m_streamSealedCount = i + 1;
		}
		++i;
	}
}

void AgentMessageUnit::updateLiveThinking(const QString& content)
{
	if (m_liveThinkingBlock < 0) {
		const int index = m_thinkingBlocks.size();
		const bool wasExpanded = m_expandedThinkingIndices.contains(index);
		m_thinkingBlocks.append({ content, wasExpanded });

		Segment segment;
		segment.type = Segment::Thinking;
		segment.thinkingIndex = index;
		m_segments.append(segment);

		insertThinking(index);
		m_liveThinkingBlock = index;
		return;
	}

	ThinkingBlock& block = m_thinkingBlocks[m_liveThinkingBlock];
	if (block.content == content)
		return;
	block.content = content;

	if (block.card) {
		if (QLabel* header = block.card->findChild<QLabel*>(QStringLiteral("agentThinkHeader"))) {
			const QString arrow = block.expanded ? QStringLiteral("▼") : QStringLiteral("▶");
			header->setText(thinkingAnchorHtml(m_liveThinkingBlock, arrow, thinkingPreview(content).toHtmlEscaped()));
		}
	}
	if (block.expanded && block.body) {
		block.body->setHtml(thinkingBodyHtml(content));
	}
	if (!m_bulkFit)
		updateHeightToContent();
}

void AgentMessageUnit::sealLiveThinking()
{
	if (m_liveThinkingSegment < 0)
		return;
	const int sealedThrough = m_liveThinkingSegment + 1;
	if (m_streamSealedCount < sealedThrough)
		m_streamSealedCount = sealedThrough;
	m_liveThinkingSegment = -1;
	m_liveThinkingBlock = -1;
}

void AgentMessageUnit::renderLiveReply(const QString& markdown)
{
	// 文本没变且区域已渲染过则跳过
	if (markdown == m_liveRenderedText && m_liveSegmentEntry >= 0)
		return;

	QElapsedTimer timer;
	timer.start();

	// m_segments 只保留该 Reply 的最新全文，供 rebuild / 主题重载重放
	if (m_liveSegmentEntry < 0) {
		Segment segment;
		segment.type = Segment::Markdown;
		segment.text = markdown;
		m_segments.append(segment);
		m_liveSegmentEntry = static_cast<int>(m_segments.size()) - 1;
	}
	else {
		m_segments[m_liveSegmentEntry].text = markdown;
	}

	// 删掉上次 live 区域（m_liveLayoutMark 之后）的部件，并从 m_proseViews 摘除
	if (m_liveLayoutMark >= 0) {
		while (m_partsLayout->count() > m_liveLayoutMark) {
			QLayoutItem* item = m_partsLayout->takeAt(m_liveLayoutMark);
			if (QWidget* widget = item ? item->widget() : nullptr) {
				if (auto* prose = qobject_cast<QTextBrowser*>(widget))
					m_proseViews.removeOne(prose);
				widget->hide();
				widget->deleteLater();
			}
			delete item;
		}
	}
	else {
		m_liveLayoutMark = m_partsLayout->count();
	}

	m_rebuilding = true;
	insertMarkdownWithCodeShadow(markdown);
	m_rebuilding = false;
	m_liveRenderedText = markdown;
	if (!m_bulkFit)
		updateHeightToContent();

	traceRender("renderLiveReply", timer.elapsed());
}

void AgentMessageUnit::closeLiveReply()
{
	m_liveIndex = -1;
	m_liveLayoutMark = -1;
	m_liveSegmentEntry = -1;
	m_liveRenderedText.clear();
}

void AgentMessageUnit::resetLiveState()
{
	m_streamSealedCount = 0;
	closeLiveReply();
	m_liveThinkingSegment = -1;
	m_liveThinkingBlock = -1;
}

void AgentMessageUnit::syncLiveAfterRebuild()
{
	if (m_liveIndex < 0)
		return;
	m_liveLayoutMark = m_partsLayout->count();
	if (m_liveSegmentEntry >= 0 && m_liveSegmentEntry < m_segments.size())
		m_liveRenderedText = m_segments[m_liveSegmentEntry].text;
	else
		m_liveRenderedText.clear();
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
	resetLiveState(); // 只清簿记，已渲染的部件保留（对应已完成的消息）
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
	if (!m_bulkFit)
		updateHeightToContent();
	syncLiveAfterRebuild();
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

	header->setText(thinkingAnchorHtml(index, arrow, thinkingPreview(block.content).toHtmlEscaped()));
	layout->addWidget(header);

	if (block.expanded) {
		QTextBrowser* body = createRichPart(QStringLiteral("agentThinkBody"));
		block.body = body;
		body->setHtml(thinkingBodyHtml(block.content));
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
		header->setText(thinkingAnchorHtml(index, arrow, thinkingPreview(block.content).toHtmlEscaped()));
	}

	QVBoxLayout* cardLayout = qobject_cast<QVBoxLayout*>(block.card->layout());
	if (block.expanded && !block.body) {
		QTextBrowser* body = createRichPart(QStringLiteral("agentThinkBody"));
		block.body = body;
		body->setHtml(thinkingBodyHtml(block.content));
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

	if (!m_bulkFit)
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

	// 就地展开/收起，不整条重建（避免抖动）；异常时兜底
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

	header->setText(toolAnchorHtml(index, arrow, block.title.toHtmlEscaped()));
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
		header->setText(toolAnchorHtml(index, arrow, block.title.toHtmlEscaped()));
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

	if (!m_bulkFit)
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
			// 思考/工具卡片：取标题（去 HTML 标签）与展开正文
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
			"background:") + ThemeManager::instance().inputBg() + QStringLiteral(
				";border:1px solid ") + ThemeManager::instance().border() + QStringLiteral(
					";box-shadow:0 1px 2px ")
			+ ThemeManager::instance().color(QStringLiteral("shadowSubtle"))
			+ QStringLiteral(";"
				"border-radius:4px;"
				"padding:1px 4px;"
				"font-family:Consolas,Menlo,monospace;"
				"font-size:0.9em;"
				"color:") + ThemeManager::instance().textPrimary() + QStringLiteral(
					";'>%1</span>"
				).arg(codes.at(i).toHtmlEscaped());
		result.replace(token, span);
	}

	return result;
}
