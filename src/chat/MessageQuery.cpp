#include "MessageQuery.h"
#include "AgentMessageUnit.h"
#include "DshEventParser.h"
#include "DshApiClient.h"
#include "CacheHistoryManager.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
	// 历史日志里混有大量"流式输出分片"类事件（reasoning-chunks、
	// assistant/chunk、text-chunks、tool-call-chunks、step/* 等）——
	// 它们只是服务端记录的增量产物，渲染时永远不会被用到（历史回放只认
	// user/assistant 消息与 tool/call+result）。若不过滤，像 12198 条那样
	// 的日志会按 5 条/批切出两千多轮事件循环，光分批开销就吃掉一秒多。
	// 这里只保留真正会渲染的四类事件，其余整类丢弃。
	QJsonArray compactHistoryEvents(const QJsonArray& events)
	{
		QJsonArray kept;
		for (const auto& value : events) {
			QJsonObject event = value.toObject();
			if (event.contains(QStringLiteral("event")) && event.value(QStringLiteral("event")).isObject())
				event = event.value(QStringLiteral("event")).toObject();
			const QString type = event.value(QStringLiteral("type")).toString();
			if (type == QStringLiteral("user/message")
				|| type == QStringLiteral("assistant/message")
				|| type == QStringLiteral("tool/call")
				|| type == QStringLiteral("tool/result")) {
				kept.append(value);
			}
		}
		if (kept.size() != events.size()) {
			qInfo().noquote() << "[MessageQuery] compacted history events:" << events.size()
				<< "->" << kept.size() << "(dropped streaming chunk/step noise)";
		}
		return kept;
	}

	QPushButton* makeCopyButton(QWidget* widget)
	{
		auto* button = new QPushButton(QString(QChar(0x29C9))); // ⧉ 复制图标
		button->setToolTip(QCoreApplication::translate("MessageQuery", "复制"));
		button->setFixedSize(28, 24);
		button->setCursor(Qt::PointingHandCursor);
		button->setObjectName(QStringLiteral("msgCopyButton")); // 外观规则见 resources/styles/chat.qss（#chatScrollContent QPushButton#msgCopyButton）
		// 复制内容：Agent 气泡（容器化后不再是 QTextBrowser）用 textContent() 取
		// “正文 + 代码”按顺序的纯文本；其余消息单元是 QTextBrowser 子类，走 toPlainText()
		QObject::connect(button, &QPushButton::clicked, widget, [widget]() {
			QString text;
			if (auto* agent = qobject_cast<AgentMessageUnit*>(widget))
				text = agent->textContent();
			else if (auto* browser = qobject_cast<QTextBrowser*>(widget))
				text = browser->toPlainText();
			if (!text.isEmpty())
				QGuiApplication::clipboard()->setText(text);
			});
		return button;
	}

	bool isHiddenContextUserMessage(const QJsonObject& event)
	{
		const QJsonObject data = event.value(QStringLiteral("data")).toObject();
		QJsonObject message = data.value(QStringLiteral("message")).toObject();
		if (message.isEmpty())
			message = data;

		const QJsonObject source = message.value(QStringLiteral("source")).toObject();
		const QString form = source.value(QStringLiteral("form")).toString();

		// 这些是 DSH 注入的运行时上下文/系统提示，不是用户真正发送的消息
		return form == QStringLiteral("snapshot")
			|| form == QStringLiteral("instructions")
			|| form == QStringLiteral("catalog");
	}

	void appendHistoryEventsToQuery(MessageQuery* query, QVBoxLayout* layout, const QJsonArray& events)
	{
		for (const auto& value : events) {
			QJsonObject event = value.toObject();
			if (event.contains(QStringLiteral("event")) && event.value(QStringLiteral("event")).isObject())
				event = event.value(QStringLiteral("event")).toObject();

			const QString type = event.value(QStringLiteral("type")).toString();
			if (type == QStringLiteral("user/message")) {
				if (isHiddenContextUserMessage(event))
					continue;
				const QString text = extractEventText(event).trimmed();
				if (!text.isEmpty())
					query->addUserMessage(text, layout);
			}
			else if (type == QStringLiteral("assistant/message")) {
				const QString thinking = extractThinking(event);
				const QString reply = extractReply(event);
				if (!reply.isEmpty() || !thinking.isEmpty())
					query->addAgentMessage(reply, layout, thinking);
			}
			else if (type == QStringLiteral("tool/call")) {
				const ToolCallInfo tool = extractToolCall(event);
				if (tool.valid) {
					AgentMessageUnit* target = query->lastAgentUnitIfLast();
					if (!target)
						target = query->addAgentMessage(QString(), layout);
					const QString html = QStringLiteral("<pre style='white-space:pre-wrap;word-break:break-all;margin:0;'>%1</pre>")
						.arg(QString::fromUtf8(
							QJsonDocument(tool.arguments).toJson(QJsonDocument::Indented))
							.toHtmlEscaped());
					target->appendToolCall(tool.name, html);
				}
			}
			else if (type == QStringLiteral("tool/result")) {
				const ToolResultInfo result = extractToolResult(event);
				if (result.valid) {
					AgentMessageUnit* target = query->lastAgentUnitIfLast();
					if (!target)
						target = query->addAgentMessage(QString(), layout);
					const QString html = QStringLiteral("<pre style='white-space:pre-wrap;word-break:break-all;margin:0;'>%1</pre>").arg(result.message.toHtmlEscaped());
					target->appendToolResult(html);
				}
			}
		}
	}
}

MessageQuery::MessageQuery() = default;

MessageQuery::~MessageQuery()
{
	clear();
}

UserMessageUnit* MessageQuery::addUserMessage(const QString& text, QVBoxLayout* layout)
{
	auto* unit = new UserMessageUnit;
	unit->setMessage(text);

	// 消息下方放复制按钮，整体右对齐
	auto* container = new QWidget;
	auto* box = new QVBoxLayout(container);
	box->setContentsMargins(0, 0, 0, 0);
	box->setSpacing(2);

	box->addWidget(unit, 0, Qt::AlignRight);

	box->addWidget(makeCopyButton(unit), 0, Qt::AlignRight);

	layout->addWidget(container, 0, Qt::AlignRight);
	messages.push_back(MessageUnit{ 0, nullptr, unit, nullptr, container });
	return unit;
}

SystemMessageUnit* MessageQuery::addSystemMessage(const QString& text, QVBoxLayout* layout)
{
	auto* unit = new SystemMessageUnit;
	unit->setMessage(text);
	layout->addWidget(unit, 0, Qt::AlignLeft);
	messages.push_back(MessageUnit{ 2, nullptr, nullptr, unit, nullptr });
	return unit;
}

AgentMessageUnit* MessageQuery::addAgentMessage(const QString& markdown, QVBoxLayout* layout,
	const QString& thinking)
{
	// 如果上一条仍然是 Agent 消息，且中间没有被用户/系统消息打断，就合并到同一个气泡
	if (AgentMessageUnit* last = lastAgentUnitIfLast()) {
		// 每次合并新的一段输出前，先加一个段落分隔，避免多段内容挤在一起。
		// 如果这一段只有回复没有思考，appendMarkdownWithCodeShadow 内部会自己分段。
		if (last->hasContent() && !thinking.isEmpty())
			last->appendSeparator();

		if (!thinking.isEmpty())
			last->appendThinking(thinking);
		if (!markdown.isEmpty())
			last->appendMarkdownWithCodeShadow(markdown);
		return last;
	}

	auto* unit = new AgentMessageUnit;
	unit->setBulkFit(m_bulkFitting);
	// AgentMessageUnit 外观见 resources/styles/chat.qss：#agentBubble 内的 #agentUnit 由下方容器规则覆盖为透明

	if (!thinking.isEmpty())
		unit->appendThinking(thinking);

	unit->appendMarkdownWithCodeShadow(markdown);

	// 整个容器作为白色圆角气泡，工具调用也会显示在气泡内部
	auto* container = new QWidget;
	container->setObjectName(QStringLiteral("agentBubble"));
	container->setAttribute(Qt::WA_StyledBackground, true);
	// 气泡容器外观见 resources/styles/chat.qss（#agentBubble）
	auto* box = new QVBoxLayout(container);
	box->setContentsMargins(8, 8, 8, 8);
	box->setSpacing(2);

	box->addWidget(unit, 0, Qt::AlignLeft);

	box->addWidget(makeCopyButton(unit), 0, Qt::AlignLeft);

	layout->addWidget(container, 0, Qt::AlignLeft);
	messages.push_back(MessageUnit{ 1, unit, nullptr, nullptr, container });
	return unit;
}

AgentMessageUnit* MessageQuery::lastAgentUnit() const
{
	for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
		if (it->agentUnit)
			return it->agentUnit;
	}
	return nullptr;
}

AgentMessageUnit* MessageQuery::lastAgentUnitIfLast() const
{
	if (messages.empty())
		return nullptr;

	// 只有最后一条消息是 Agent 消息时才返回，保证连续 Agent 回复可以合并到同一个气泡
	return messages.back().agentUnit;
}

void MessageQuery::detachFromLayout(QVBoxLayout* layout)
{
	for (const MessageUnit& message : messages) {
		if (message.container) {
			layout->removeWidget(message.container);
			message.container->hide();
		}
		else if (message.systemUnit) {
			layout->removeWidget(message.systemUnit);
			message.systemUnit->hide();
		}
	}
}

void MessageQuery::attachToLayout(QVBoxLayout* layout)
{
	for (const MessageUnit& message : messages) {
		if (message.container) {
			const Qt::Alignment align = message.type == 0 ? Qt::AlignRight : Qt::AlignLeft;
			layout->addWidget(message.container, 0, align);
			message.container->show();
		}
		else if (message.systemUnit) {
			layout->addWidget(message.systemUnit, 0, Qt::AlignLeft);
			message.systemUnit->show();
		}
	}
}

void MessageQuery::releaseWidgets()
{
	for (const MessageUnit& message : messages) {
		if (message.container)
			message.container->setParent(nullptr);
		else if (message.systemUnit)
			message.systemUnit->setParent(nullptr);
	}
}

void MessageQuery::prependQuery(QVBoxLayout* layout, MessageQuery* older, int layoutIndex)
{
	if (!older)
		return;
	for (const MessageUnit& message : older->messages) {
		if (message.container) {
			const Qt::Alignment align = message.type == 0 ? Qt::AlignRight : Qt::AlignLeft;
			layout->insertWidget(layoutIndex, message.container, 0, align);
			message.container->show();
		}
		else if (message.systemUnit) {
			layout->insertWidget(layoutIndex, message.systemUnit, 0, Qt::AlignLeft);
			message.systemUnit->show();
		}
		++layoutIndex;
	}

	// 把 older 的单元移到本列表头部，并接管其所有权
	if (!older->messages.empty()) {
		messages.insert(messages.begin(), older->messages.begin(), older->messages.end());
		older->messages.clear();
	}
	delete older;
}

void MessageQuery::clear()
{
	for (const MessageUnit& message : messages) {
		if (message.container) {
			// 用户/Agent 消息带容器，删除容器会连带删除里面的文本单元和复制按钮
			delete message.container;
		}
		else {
			if (message.userUnit)
				delete message.userUnit;
			if (message.agentUnit)
				delete message.agentUnit;
			if (message.systemUnit)
				delete message.systemUnit;
		}
	}
	messages.clear();
}

void MessageQuery::appendEvents(QVBoxLayout* layout, const QJsonArray& events)
{
	appendHistoryEventsToQuery(this, layout, compactHistoryEvents(events));
}

MessageQuery::StreamFrameResult MessageQuery::applyStreamEvent(
	const QJsonObject& event, QVBoxLayout* layout, bool wasStreaming)
{
	StreamFrameResult result;
	const QString eventType = event.value(QStringLiteral("type")).toString();

	// assistant/message：流式收尾。若之前通过 chunk 流式显示过则不再重复追加，
	// 只封存尾部气泡；否则把积压工具 flush 后再按需追加正文。
	if (eventType == QStringLiteral("assistant/message")) {
		result.kind = StreamFrameResult::FinalMessage;

		AgentMessageUnit* target = lastAgentUnitIfLast();
		if (target)
			target->flushStream();

		if (!wasStreaming) {
			const QString thinking = extractThinking(event);
			const QString reply = extractReply(event);

			if (!thinking.isEmpty() || !reply.isEmpty()) {
				if (target) {
					// 上一条仍是 Agent 消息：继续追加进同一气泡保持连续
					if (!thinking.isEmpty())
						target->appendThinking(thinking);
					if (!reply.isEmpty())
						target->appendMarkdownWithCodeShadow(reply);
				}
				else {
					addAgentMessage(reply, layout, thinking);
				}
				result.contentRouted = true;
			}
		}
		return result;
	}

	// assistant/chunk（增量文本/思考分片）
	if (eventType == QStringLiteral("assistant/chunk")) {
		const QString chunk = extractEventText(event);
		const QString chunkType = extractChunkType(event);

		AgentMessageUnit* streamTarget = lastAgentUnitIfLast();
		if (!streamTarget)
			streamTarget = addAgentMessage(QString(), layout);

		if (chunkType == QStringLiteral("reasoning-delta") && !chunk.isEmpty()) {
			streamTarget->appendStreamChunk(StreamSegment::Thinking, chunk);
			result.contentRouted = true;
		}
		else if (chunkType == QStringLiteral("text-delta") && !chunk.isEmpty()) {
			streamTarget->appendStreamChunk(StreamSegment::Reply, chunk);
			result.contentRouted = true;
		}
		result.kind = StreamFrameResult::Streaming;
		return result;
	}

	// 批量分片（一次携带多段）
	if (eventType == QStringLiteral("text-chunks") || eventType == QStringLiteral("reasoning-chunks")) {
		const QJsonObject eventData = event.value(QStringLiteral("data")).toObject();
		const QJsonArray texts = eventData.value(QStringLiteral("texts")).toArray();
		if (texts.isEmpty())
			return result;

		AgentMessageUnit* target = lastAgentUnitIfLast();
		if (!target)
			target = addAgentMessage(QString(), layout);

		const bool thinking = eventType == QStringLiteral("reasoning-chunks");
		for (const auto& value : texts) {
			const QString chunk = value.toString();
			if (chunk.isEmpty())
				continue;
			target->appendStreamChunk(
				thinking ? StreamSegment::Thinking : StreamSegment::Reply, chunk);
			result.contentRouted = true;
		}
		result.kind = StreamFrameResult::Streaming;
		return result;
	}

	// 工具调用/结果：即使没有 assistant/chunk 也要显示
	if (eventType == QStringLiteral("tool/call")) {
		const ToolCallInfo tool = extractToolCall(event);
		if (tool.valid) {
			AgentMessageUnit* target = lastAgentUnitIfLast();
			if (!target)
				target = addAgentMessage(QString(), layout);
			const QString html = QStringLiteral("<pre>%1</pre>")
				.arg(QString::fromUtf8(
					QJsonDocument(tool.arguments).toJson(QJsonDocument::Indented))
					.toHtmlEscaped());
			target->appendStreamChunk(StreamSegment::ToolCall, html, tool.name);
			result.kind = StreamFrameResult::Streaming;
			result.contentRouted = true;
		}
		return result;
	}
	if (eventType == QStringLiteral("tool/result")) {
		const ToolResultInfo toolResult = extractToolResult(event);
		if (toolResult.valid) {
			AgentMessageUnit* target = lastAgentUnitIfLast();
			if (!target)
				target = addAgentMessage(QString(), layout);
			const QString html = QStringLiteral("<pre>%1</pre>").arg(toolResult.message.toHtmlEscaped());
			target->appendStreamChunk(StreamSegment::ToolResult, html);
			result.kind = StreamFrameResult::Streaming;
			result.contentRouted = true;
		}
		return result;
	}

	// 其它（user/message 已在发送时上屏、未知类型等）：不需要更新气泡
	return result;
}

MessageQuery* MessageQuery::fromEvents(const QJsonArray& events)
{
	// 与 HistoryLoader 的增量构建共用同一条 MessageQueryBuilder 管线：
	// compact 滤噪音 → 批量构建（bulk 拟合）→ 统一收尾。这里只是同步“抽干”
	// 全部批次后取结果（调用方负责后续 attach/释放）。
	MessageQueryBuilder builder;
	builder.start(events);
	while (builder.step()) {
		// 同步 drain：预构建内容通常很小，不需要按事件循环让步
	}
	return builder.takeResult();
}

// ------------------------------------------------------------------
// MessageQueryBuilder
// ------------------------------------------------------------------

MessageQueryBuilder::MessageQueryBuilder() = default;

MessageQueryBuilder::~MessageQueryBuilder()
{
	cancel();
}

void MessageQueryBuilder::start(const QJsonArray& events)
{
	cancel();

	m_holder = new QWidget;
	m_layout = new QVBoxLayout(m_holder);
	m_query = new MessageQuery;
	// 批量构建：新建气泡先抑制逐次高度拟合，整批完成后统一拟合一次
	m_query->setBulkFitting(true);
	// 先整段压缩（滤掉流式分片噪音）再分批构建：
	// 否则会被上万条分片事件切成两千多轮事件循环。
	m_events = compactHistoryEvents(events);
	m_index = 0;
	m_active = true;
}

bool MessageQueryBuilder::step(int batchSize)
{
	if (!m_active || !m_query || !m_layout)
		return false;

	const int end = qMin(m_index + batchSize, m_events.size());
	QJsonArray batch;
	for (int i = m_index; i < end; ++i)
		batch.append(m_events.at(i));

	m_query->appendEvents(m_layout, batch);
	m_index = end;

	if (m_index < m_events.size())
		return true;

	// 构建完成：关闭批量模式，对每个 Agent 气泡统一做一次高度拟合
	// （避免逐条 append 时 O(气泡内片段数) 的 refit 反复执行 + 定时器风暴）
	m_query->setBulkFitting(false);
	for (MessageQuery::MessageUnit& message : m_query->messages) {
		if (message.agentUnit) {
			message.agentUnit->setBulkFit(false);
			message.agentUnit->updateHeightToContent();
		}
	}

	// 摘下控件并释放临时容器，m_query 留给 takeResult()
	m_query->detachFromLayout(m_layout);
	m_query->releaseWidgets();
	delete m_holder;
	m_holder = nullptr;
	m_layout = nullptr;
	m_active = false;
	m_events = QJsonArray();
	m_index = 0;
	return false;
}

void MessageQueryBuilder::cancel()
{
	if (m_query && m_layout) {
		m_query->detachFromLayout(m_layout);
		m_query->releaseWidgets();
	}
	delete m_holder;
	delete m_query;
	m_holder = nullptr;
	m_layout = nullptr;
	m_query = nullptr;
	m_events = QJsonArray();
	m_index = 0;
	m_active = false;
}

bool MessageQueryBuilder::isActive() const
{
	return m_active;
}

MessageQuery* MessageQueryBuilder::takeResult()
{
	MessageQuery* result = m_query;
	m_query = nullptr;
	m_holder = nullptr;
	m_layout = nullptr;
	m_events = QJsonArray();
	m_index = 0;
	m_active = false;
	return result;
}

// ------------------------------------------------------------------
// HistoryLoader
// ------------------------------------------------------------------

HistoryLoader::HistoryLoader(DshApiClient* api,
	MessageQuery* messages,
	QVBoxLayout* layout,
	HistoryManager* history,
	QScrollArea* scrollArea,
	QObject* parent)
	: QObject(parent)
	, m_api(api)
	, m_messages(messages)
	, m_layout(layout)
	, m_history(history)
	, m_scrollArea(scrollArea)
{
}

namespace
{
	// 0.1.5：session/page 与 session/follow 的记录都是
	// {type:"event", event:{type,seq,time,data,…}}，这里取出内层事件，
	// 交给既有的分批构建路径（它只认裸事件对象）。
	QJsonArray eventsFromRecords(const QJsonArray& records)
	{
		QJsonArray events;
		for (const auto& record : records) {
			const QJsonObject event = record.toObject().value(QStringLiteral("event")).toObject();
			if (!event.isEmpty())
				events.append(event);
		}
		return events;
	}
	// 日志事件里的 seq（上翻分页按它做 beforeSeq 游标）
	int seqOf(const QJsonObject& event)
	{
		return event.value(QStringLiteral("seq")).toInt();
	}

	// 首屏从 follow 快照播种时最多取多少条事件。
	// 快照一页可以给几十条消息（实测 436 条事件），一次性构建会明显拖慢切会话；
	// 取最近约一页（~20 条消息）与旧版 session.history 的首屏量相当，
	// 更早的内容交给"加载更多"按 beforeSeq 上翻。
	constexpr int kSeedEventCap = 200;
}

void HistoryLoader::load(const QString& sessionId)
{
	if (sessionId.isEmpty())
		return;

	// 同一个会话已有请求在途时不要重复发起；切换会话则允许取消旧构建并重新加载新会话。
	if (sessionId == m_sessionId && m_loading)
		return;

	if (sessionId != m_sessionId) {
		m_builder.cancel();
		m_reachedEnd = false; // 新会话重新判定
		// 0.1.5：分页状态随会话重置（游标与"已播种"都属于上一个会话）
		m_seeded = false;
		m_loadPending = false;
		m_throughSeq = 0;
		m_oldestSeq = 0;
		m_contentLastSeq = 0;
	}

	m_sessionId = sessionId;
	++m_loadGeneration;
	const int generation = m_loadGeneration;
	m_loading = true;
	emit loadingChanged(true);

	const int oldScroll = m_scrollArea ? m_scrollArea->verticalScrollBar()->value() : 0;
	const int oldScrollMax = m_scrollArea ? m_scrollArea->verticalScrollBar()->maximum() : 0;
	const int oldCount = m_history ? m_history->eventCount() : 0;
	const QString requestedSessionId = sessionId;

	if (!m_api || !m_history) {
		m_loading = false;
		emit loadingChanged(false);
		return;
	}

	// 0.1.5：分页端点是 session/page，必须带 session/follow 快照给出的 throughSeq。
	// 游标还没到就先挂起（loading 保持 true，遮罩不闪），等 setStreamCursor() 唤醒。
	if (m_throughSeq <= 0) {
		m_loadPending = true;
		qInfo().noquote() << "[History] load deferred until follow cursor sessionId=" << sessionId;

		// 看门狗：follow 流断了/快照没来时不能一直转圈。
		// 有回落游标（session/list 的 projections.asOfSeq）就直接发请求，
		// 否则明确报错，让 UI 收掉 loading。
		if (!m_cursorWatchdog) {
			m_cursorWatchdog = new QTimer(this);
			m_cursorWatchdog->setSingleShot(true);
			connect(m_cursorWatchdog, &QTimer::timeout, this, [this]() {
				if (!m_loadPending || m_sessionId.isEmpty())
					return;

				if (m_fallbackCursor > 0) {
					qWarning().noquote() << "[History] follow cursor timeout -> fallback throughSeq="
						<< m_fallbackCursor << " sessionId=" << m_sessionId;
					m_loadPending = false;
					m_throughSeq = m_fallbackCursor;
					m_seeded = true; // 已有内容；随后快照若更新会照常重建
					load(m_sessionId);
					return;
				}

				qWarning().noquote() << "[History] follow cursor timeout, no fallback sessionId=" << m_sessionId;
				m_loadPending = false;
				m_loading = false;
				emit loadingChanged(false);
				emit historyError(QStringLiteral("stream-timeout"),
					QStringLiteral("等待会话事件流超时，历史暂时无法加载"));
				});
		}
		m_cursorWatchdog->start(2500);
		return;
	}

	// 记录 session/page RPC 的往返耗时（发出请求 -> 回包到达）
	QElapsedTimer requestTimer;
	requestTimer.start();

	QJsonObject address;
	address.insert(QStringLiteral("kind"), QStringLiteral("session"));
	address.insert(QStringLiteral("sessionId"), sessionId);

	QJsonObject request;
	request.insert(QStringLiteral("address"), address);
	request.insert(QStringLiteral("throughSeq"), m_throughSeq);
	request.insert(QStringLiteral("maxMessages"), m_history->limit());
	// 上翻一页：只取比当前最早一条更早的记录。
	// 首屏（含快照播种后的刷新）不带 beforeSeq，语义是"throughSeq 往前最近一页"。
	if (m_history->loadMoreRequested() && m_oldestSeq > 0)
		request.insert(QStringLiteral("beforeSeq"), m_oldestSeq);

	QJsonObject args;
	args.insert(QStringLiteral("request"), request);

	m_api->callMethod(
		QStringLiteral("session/page"),
		args,
		[this, generation, oldScroll, oldScrollMax, oldCount, requestedSessionId, requestTimer](const QJsonObject& value) {
			if (generation != m_loadGeneration)
				return;

			m_loading = false;
			emit loadingChanged(false);

			if (requestedSessionId != m_sessionId)
				return;

			const QJsonArray events = eventsFromRecords(value.value(QStringLiteral("records")).toArray());
			const bool serverHasMore = value.value(QStringLiteral("hasMore")).toBool();
			const int newCount = events.size();

			const bool paging = m_history->loadMoreRequested();

			// 头部插入会改变内容高度。为避免“闪到底部/位置漂移”，
			// 先记录视口顶部附近某条已有消息作锚点，插入后再保持偏移。
			// “加载更多”按钮在列表顶部：用户本就在顶部(value≈0)时不做锚点校正
			// （Qt 保持 value=0，新插入的老消息自然出现在视口顶部）。
			auto captureScrollAnchor = [this]() {
				QScrollBar* bar = m_scrollArea ? m_scrollArea->verticalScrollBar() : nullptr;
				const int beforeVal = bar ? bar->value() : 0;
				const bool userAtTop = bar && beforeVal <= 8;

				m_pendingAnchorValid = false;
				if (userAtTop || !bar || !m_scrollArea->widget() || !m_layout)
					return;

				for (int i = 0; i < m_layout->count(); ++i) {
					QLayoutItem* item = m_layout->itemAt(i);
					QWidget* w = item ? item->widget() : nullptr;
					if (!w || !w->isVisible())
						continue;
					const int top = w->geometry().top();
					const int bottom = top + w->geometry().height();
					if (bottom > beforeVal) {
						m_pendingAnchor = w;
						m_pendingKeep = top - beforeVal;
						m_pendingAnchorValid = true;
						break;
					}
				}
				if (!m_pendingAnchorValid && m_layout->count() > 0) {
					if (QLayoutItem* item = m_layout->itemAt(m_layout->count() - 1)) {
						if (QWidget* w = item->widget()) {
							m_pendingAnchor = w;
							m_pendingKeep = w->geometry().top() - beforeVal;
							m_pendingAnchorValid = true;
						}
					}
				}
				};

			if (!paging) {
				// 到顶判定只用“服务端回包数量”：拉不到比已有更多的条数才算到顶。
				// 用 hasMore 判定在部分流程会误判（有更多却被当成没有更多）。
				m_reachedEnd = !(newCount > oldCount);
			}

			qInfo().noquote() << "[History] events arrived sessionId=" << requestedSessionId
				<< "old=" << oldCount << "new=" << newCount
				<< "paging=" << paging << "oldestSeq=" << m_oldestSeq
				<< "roundTripMs=" << requestTimer.elapsed();

			if (paging) {
				// 0.1.5 上翻一页：带 beforeSeq 的请求返回的全部是更早的事件，
				// 所以整页插到顶部（不再像旧版那样比较条数取差集）。
				if (newCount == 0) {
					m_reachedEnd = true;
					m_history->setHasMore(false);
				}
				else {
					m_oldestSeq = seqOf(events.at(0).toObject());
					m_history->setEventCount(oldCount + newCount);
					m_history->setHasMore(serverHasMore);
					m_reachedEnd = !serverHasMore;

					if (m_messages && m_layout) {
						// 更早的一页先离屏分批构建（5 条/事件循环轮），完成后整页插到顶部
						captureScrollAnchor();
						startPrepend(events);
					}
				}
			}
			else if (oldCount == 0) {
				// 首屏历史已到达：收遮罩交给 startBuild → continueBuild（内容真正上屏那一刻），
				// 这里提前收只会先露出空白聊天区
				m_history->setEventCount(newCount);
				m_history->setHasMore(serverHasMore);
				emit loadMoreButtonVisibleChanged(serverHasMore);
				startBuild(events);
			}
			else if (newCount > oldCount) {
				m_history->setEventCount(newCount);
				m_history->setHasMore(serverHasMore);
				m_contentLastSeq = seqOf(events.at(events.size() - 1).toObject());

				captureScrollAnchor();

				// 更早的一页先离屏分批构建，完成后再一次性插到列表顶部
				QJsonArray olderEvents;
				const int olderCount = newCount - oldCount;
				for (int i = 0; i < olderCount && i < events.size(); ++i)
					olderEvents.append(events.at(i));
				if (m_messages && m_layout && !olderEvents.isEmpty())
					startPrepend(olderEvents);
			}
			else {
				m_history->setHasMore(serverHasMore);
			}

			// 服务端确认没有更多：弹 toast（若本次点击请求过）；按钮仍保持原样
			if (m_history->loadMoreRequested() && !m_history->hasMore())
				emit noMoreHistory();
			m_history->setLoadMoreRequested(false);

			// 按钮显隐以 hasMore 为准；DSHHub 侧在列表非空时会保持按钮原样，
			// 不因 hide/toast 造成整列重排。
			if (!m_builder.isActive())
				emit loadMoreButtonVisibleChanged(m_history ? m_history->hasMore() : false);
		},
		[this, generation, requestedSessionId, requestTimer](const DshApiClient::RpcError& error) {
			if (generation != m_loadGeneration)
				return;

			m_loading = false;
			emit loadingChanged(false);
			if (requestedSessionId != m_sessionId)
				return;
			qWarning().noquote() << "[History] session/page failed after"
				<< requestTimer.elapsed() << "ms sessionId=" << requestedSessionId
				<< "error=" << error.code << error.message;
			emit historyError(error.code, error.message);
		});
}

/**
 * 用缓存的时间点接管分页状态（缓存内容仍有效时调用）。
 *
 * 与 adoptSession() 的分工：adoptSession 只把 loader 绑到会话上、清空分页状态；
 * 本函数再把缓存建立时的游标/最早 seq/内容最新 seq 装回去，于是随后到达的
 * follow 快照只要不比缓存内容新，就不会触发重建——缓存命中就是真的秒开。
 *
 * @param contentLastSeq 缓存内容里最新一条事件的 seq（包含游标之后的实时事件），
 *                       新鲜度就以它为界：快照 cursor ≤ 它即视为没有新内容。
 */
void HistoryLoader::adoptCachedState(int throughSeq, int oldestSeq, int contentLastSeq,
	int eventCount, bool hasMore)
{
	if (throughSeq > 0)
		m_throughSeq = throughSeq;
	if (oldestSeq > 0)
		m_oldestSeq = oldestSeq;
	if (contentLastSeq > 0)
		m_contentLastSeq = contentLastSeq;

	m_seeded = true;
	m_loadPending = false;
	m_loading = false;
	m_reachedEnd = !hasMore;

	if (m_history) {
		m_history->setEventCount(eventCount);
		m_history->setHasMore(hasMore);
	}

	qInfo().noquote() << "[History] adopted cached pagination state sessionId=" << m_sessionId
		<< "throughSeq=" << m_throughSeq << "oldestSeq=" << m_oldestSeq
		<< "contentLastSeq=" << m_contentLastSeq
		<< "count=" << eventCount << "hasMore=" << hasMore;
}

/**
 * 收到 session/follow 快照给出的游标：记下来，并唤醒之前挂起的首屏请求。
 */
void HistoryLoader::setStreamCursor(int cursor)
{
	if (cursor <= 0)
		return;

	m_throughSeq = cursor;
	if (m_cursorWatchdog)
		m_cursorWatchdog->stop(); // 游标到了，看门狗下班

	if (m_loadPending && !m_sessionId.isEmpty()) {
		m_loadPending = false;
		load(m_sessionId);
	}
}

/**
 * 设置 follow 快照没到时的 throughSeq 回落值（session/list 的 projections.asOfSeq）。
 */
void HistoryLoader::setFallbackCursor(int cursor)
{
	m_fallbackCursor = cursor > 0 ? cursor : 0;
}

/**
 * 用"预取的一页事件"播种首屏（路 1：不贴控件树，走分批构建）。
 *
 * 这是"首次进入会话"的快速路径：事件已经在内存里（预取回来的），
 * 所以既不用等 session/follow，也不用一次性布局整棵预构建的树。
 */
void HistoryLoader::seedFromPrefetched(const QString& sessionId, const QJsonArray& events,
	int throughSeq, bool hasMore)
{
	if (sessionId.isEmpty() || sessionId != m_sessionId || events.isEmpty())
		return;

	if (throughSeq > 0)
		m_throughSeq = throughSeq;
	m_oldestSeq = seqOf(events.at(0).toObject());
	m_contentLastSeq = throughSeq > 0 ? throughSeq : m_oldestSeq;
	m_seeded = true;
	m_loadPending = false;
	m_loading = false;
	if (m_cursorWatchdog)
		m_cursorWatchdog->stop(); // 内容已经在了，不必再等游标
	m_reachedEnd = !hasMore;

	qInfo().noquote() << "[History] seeded from prefetched page sessionId=" << sessionId
		<< "events=" << events.size() << "throughSeq=" << m_throughSeq
		<< "oldestSeq=" << m_oldestSeq << "hasMore=" << hasMore;

	// 遮罩不在"播种"这一刻收：构建全程是在隐藏容器里进行的（见 continueBuild），
	// 提前收只会露出空白聊天区（实测大页白屏 ~145ms，观感像卡死）。
	// 内容真正上屏时由 continueBuild() 统一发 firstHistoryArrived()。
	emit loadingChanged(false);
	emit loadMoreButtonVisibleChanged(hasMore);

	if (m_history) {
		m_history->setEventCount(events.size());
		m_history->setHasMore(hasMore);
	}

	// 分批构建：每轮事件循环一步，避免单帧阻塞
	startBuild(events);
}

/**
 * 用 session/follow 的快照记录播种首屏历史：省掉一次 session/page 往返。
 * 不是当前会话、或已经播种过，则忽略。
 */
void HistoryLoader::seedFromSnapshot(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore)
{
	if (sessionId.isEmpty() || sessionId != m_sessionId)
		return; // 切走后迟到的快照
	// 已有内容（快照播种过，或刚恢复了缓存）：只有快照比已有内容更新时才需要重建。
	// 内容的最新位置取 m_contentLastSeq（缓存里可能含游标之后的实时事件），
	// 没有它才退回游标；快照 cursor 不超过这个位置就说明期间没有新事件 ——
	// 直接沿用现有内容，避免"命中却仍重建"。
	const int contentCut = m_contentLastSeq > 0 ? m_contentLastSeq : m_throughSeq;
	if (m_seeded && cursor > 0 && cursor <= contentCut) {
		qInfo().noquote() << "[History] follow snapshot matches existing content, keep it sessionId=" << sessionId
			<< "snapshotCursor=" << cursor << "contentCut=" << contentCut;
		return;
	}

	if (cursor > 0)
		m_throughSeq = cursor;
	m_loadPending = false;
	if (m_cursorWatchdog)
		m_cursorWatchdog->stop(); // 快照到了，看门狗下班
	m_seeded = true;
	m_loading = false;
	emit loadingChanged(false);

	QJsonArray events = eventsFromRecords(records);

	// 只播种最近 kSeedEventCap 条：快照一页可能给几十条消息的事件，
	// 全量构建会让切会话明显变慢；更早的用"加载更多"按 beforeSeq 上翻。
	bool truncated = false;
	if (events.size() > kSeedEventCap) {
		QJsonArray tail;
		for (int i = events.size() - kSeedEventCap; i < events.size(); ++i)
			tail.append(events.at(i));
		events = tail;
		truncated = true;
	}

	m_oldestSeq = events.isEmpty() ? 0 : seqOf(events.at(0).toObject());
	// 内容最新位置：快照记录本身就到此为止（实时事件随后才追加）
	m_contentLastSeq = cursor > 0 ? cursor : m_oldestSeq;
	const bool effectiveHasMore = hasMore || truncated;

	qInfo().noquote() << "[History] seeded from follow snapshot sessionId=" << sessionId
		<< "cursor=" << m_throughSeq << "events=" << events.size()
		<< "oldestSeq=" << m_oldestSeq << "hasMore=" << effectiveHasMore
		<< "truncated=" << truncated;

	m_reachedEnd = !effectiveHasMore;
	emit loadMoreButtonVisibleChanged(effectiveHasMore);

	if (events.isEmpty() || !m_history) {
		// 没有内容可建 → 等不到"构建完成"的回调，遮罩必须在这里收，否则会一直盖着
		emit firstHistoryArrived();
		if (m_history)
			m_history->setHasMore(effectiveHasMore);
		return;
	}

	m_history->setEventCount(events.size());
	m_history->setHasMore(effectiveHasMore);
	startBuild(events);
}

void HistoryLoader::loadMore()
{
	if (m_sessionId.isEmpty() || m_loading)
		return;
	if (!m_history)
		return;
	// 上一次的构建（首屏或"加载更多"的离屏老页）尚未完成时先忽略，
	// 避免多个离屏构建交错
	if (m_builder.isActive())
		return;

	// 到顶（由服务端回包确认过“没有更多”）才本地弹 toast 并短路；
	// 其它情况一律真实请求，避免 hasMore 误判导致“有更多却加载不出来”。
	if (m_reachedEnd) {
		emit noMoreHistory();
		return;
	}

	qInfo().noquote() << "[History] loadMore sessionId=" << m_sessionId
		<< "reachedEnd=" << m_reachedEnd
		<< "hasMore=" << m_history->hasMore()
		<< "limit=" << m_history->limit()
		<< "count=" << m_history->eventCount();

	m_history->setLoadMoreRequested(true);
	// 0.1.5：不再靠"加大 maxMessages"换更早的内容（快照已经把最近一大页给了客户端，
	// 加大 maxMessages 反而可能返回更少，导致按钮点了没反应）；
	// 现在按 seq 上翻：session/page 带 beforeSeq = 当前最早一条事件的 seq。
	load(m_sessionId);
}

void HistoryLoader::setMessages(MessageQuery* messages)
{
	m_messages = messages;
}

void HistoryLoader::cancelBuild()
{
	m_builder.cancel();
	m_prependPending = false;
	m_pendingAnchorValid = false;
	m_pendingAnchor.clear();
}

void HistoryLoader::startBuild(const QJsonArray& events)
{
	startPageBuild(QStringLiteral("incremental build start"), events, /*prepend=*/false);
}

void HistoryLoader::startPrepend(const QJsonArray& olderEvents)
{
	startPageBuild(QStringLiteral("older page build start"), olderEvents, /*prepend=*/true);
}

void HistoryLoader::startPageBuild(const QString& logLabel, const QJsonArray& events, bool prepend)
{
	// 首屏构建与"加载更多"老页构建共用同一入口：builder 内部先 compact
	// 再分批（bulk 拟合），仅区分 prepend 模式与日志标签。
	m_prependPending = prepend;
	m_buildSessionId = m_sessionId;
	m_buildGeneration = m_loadGeneration;
	m_builder.start(events);

	// 记录构建耗时：从第一批开始渲染，到全部渲染完成（continueBuild 出口）
	m_buildTimer.restart();
	m_buildEventCount = events.size();
	qInfo().noquote() << "[History]" << logLabel << "sessionId=" << m_sessionId
		<< "events=" << m_buildEventCount;
	continueBuild();
}

void HistoryLoader::continueBuild()
{
	if (!m_builder.isActive())
		return;

	if (m_buildGeneration != m_loadGeneration) {
		m_builder.cancel();
		m_prependPending = false;
		return;
	}

	if (m_builder.step()) {
		QTimer::singleShot(0, this, [this]() { continueBuild(); });
		return;
	}

	MessageQuery* query = m_builder.takeResult();
	if (m_buildSessionId != m_sessionId || m_buildGeneration != m_loadGeneration) {
		delete query;
		m_prependPending = false;
		return;
	}

	qInfo().noquote() << "[History] incremental build done sessionId=" << m_sessionId
		<< "events=" << m_buildEventCount
		<< "buildMs=" << m_buildTimer.elapsed();

	if (m_prependPending) {
		// “加载更多”模式：把离屏构建好的老页一次性插到列表顶部
		m_prependPending = false;
		if (m_messages && m_layout && query)
			m_messages->prependQuery(m_layout, query, 1);
		else
			delete query;

		if (m_pendingAnchorValid)
			applyPendingAnchor();
		else
			m_pendingAnchorValid = false;
		emit loadMoreButtonVisibleChanged(m_history ? m_history->hasMore() : false);
		return;
	}

	// 内容此刻才真正上屏（构建全程是在隐藏容器里做的，见 MessageQueryBuilder::start）：
	// 到这里才通知 DSHHub 收掉初始化遮罩。
	// 不要在"播种/发起构建"时就发——那会先露出空白聊天区，实测大页白屏 ~145ms，
	// 观感上像卡死（"纯白 + 干等"）。宁可让遮罩多留这一会儿。
	emit firstHistoryArrived();
	emit incrementalBuildReady(query);
	emit loadMoreButtonVisibleChanged(m_history ? m_history->hasMore() : false);
}

void HistoryLoader::applyPendingAnchor()
{
	if (!m_scrollArea || !m_pendingAnchorValid || m_pendingAnchor.isNull())
		return;

	QPointer<QWidget> anchor = m_pendingAnchor;
	const int keep = m_pendingKeep;
	m_pendingAnchorValid = false;
	m_pendingAnchor.clear();

	// 老页插入后会改变内容高度；由于气泡 refit 高度晚一步才稳定，
	// 分多次校正直到几何稳定，保持插入前锚点相对视口顶部的偏移。
	const auto fix = [this, anchor, keep]() {
		if (!m_scrollArea || anchor.isNull())
			return;
		QScrollBar* scrollBar = m_scrollArea->verticalScrollBar();
		if (!scrollBar)
			return;
		scrollBar->setValue(qBound(0, anchor->geometry().top() - keep, scrollBar->maximum()));
		};
	QTimer::singleShot(0, this, fix);
	QTimer::singleShot(40, this, fix);
	QTimer::singleShot(120, this, fix);
}