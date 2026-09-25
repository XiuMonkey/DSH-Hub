#include "chat/MessageQuery.h"
#include "chat/AgentMessageUnit.h"
#include "network/DshEventParser.h"
#include "network/DshApiClient.h"
#include "chat/CacheHistoryManager.h"
#include "common/session/SessionCommands.h"
#include "core/ConnectionManager.h"
#include "ui/ShadowPanel.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
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
	// 消息气泡阴影档位：扩散给大了，外壳留白会变成消息间距
	const CardShadow::Spec kMessageShadow = { 6, 1, 12 };

	// 须与气泡 QSS 圆角一致
	constexpr int kMessageRadius = 12;

	// 只保留四类会渲染的事件，其余流式分片整类丢弃（不过滤会切出两千多轮事件循环）
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
		button->setToolTip(qtTrId("common_copy"));
		button->setFixedSize(28, 24);
		button->setCursor(Qt::PointingHandCursor);
		button->setObjectName(QStringLiteral("msgCopyButton"));
		// Agent 单元是容器，须用 textContent()；其余消息单元是 QTextBrowser 子类，走 toPlainText()
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

		// DSH 注入的运行时上下文/系统提示，不是用户真正发送的消息
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
					const QString html =
						QStringLiteral("<pre style='white-space:pre-wrap;word-break:break-all;margin:0;'>%1</pre>")
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
					const QString html =
						QStringLiteral("<pre style='white-space:pre-wrap;word-break:break-all;margin:0;'>%1</pre>")
							.arg(result.message.toHtmlEscaped());
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

	auto* container = new QWidget;
	auto* box = new QVBoxLayout(container);
	box->setContentsMargins(0, 0, 0, 0);
	box->setSpacing(2);

	// 复制按钮必须留在阴影外壳之外，否则会被一起包成一张卡片
	auto* bubbleShadow = new ShadowPanel(QStringLiteral("shadow"), kMessageShadow, container);
	bubbleShadow->setRadius(kMessageRadius);
	bubbleShadow->setCard(unit);

	box->addWidget(bubbleShadow, 0, Qt::AlignRight);
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

AgentMessageUnit* MessageQuery::addAgentMessage(const QString& markdown, QVBoxLayout* layout, const QString& thinking)
{
	if (AgentMessageUnit* last = lastAgentUnitIfLast()) {
		// 仅带思考时补分隔，纯回复由 appendMarkdownWithCodeShadow 内部分段
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
	if (!thinking.isEmpty())
		unit->appendThinking(thinking);
	unit->appendMarkdownWithCodeShadow(markdown);

	auto* bubble = new QWidget;
	bubble->setObjectName(QStringLiteral("agentBubble"));
	bubble->setAttribute(Qt::WA_StyledBackground, true);
	auto* box = new QVBoxLayout(bubble);
	box->setContentsMargins(8, 8, 8, 8);
	box->setSpacing(2);
	box->addWidget(unit, 0, Qt::AlignLeft);

	// 复制按钮同样放在阴影外壳之外，免得被包进阴影轮廓
	auto* bubbleShadow = new ShadowPanel(QStringLiteral("shadow"), kMessageShadow);
	bubbleShadow->setRadius(kMessageRadius);
	bubbleShadow->setCard(bubble);

	auto* container = new QWidget;
	auto* outerBox = new QVBoxLayout(container);
	outerBox->setContentsMargins(0, 0, 0, 0);
	outerBox->setSpacing(2);
	outerBox->addWidget(bubbleShadow, 0, Qt::AlignLeft);
	outerBox->addWidget(makeCopyButton(unit), 0, Qt::AlignLeft);
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
	// 只有最后一条是 Agent 消息才返回，保证连续回复能合并到同一气泡
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
			// 删除容器会连带删除里面的文本单元和复制按钮
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

	// assistant/message：流式收尾，已流式显示过就不重复追加，只封存尾部气泡
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
					// 上一条仍是 Agent 消息则继续追加进同一气泡
					if (!thinking.isEmpty())
						target->appendThinking(thinking);
					if (!reply.isEmpty())
						target->appendMarkdownWithCodeShadow(reply);
				}
				else {
					addAgentMessage(reply, layout, thinking);
				}
			}
		}
		return result;
	}

	if (eventType == QStringLiteral("assistant/chunk")) {
		const QString chunk = extractEventText(event);
		const QString chunkType = extractChunkType(event);

		AgentMessageUnit* streamTarget = lastAgentUnitIfLast();
		if (!streamTarget)
			streamTarget = addAgentMessage(QString(), layout);

		if (chunkType == QStringLiteral("reasoning-delta") && !chunk.isEmpty()) {
			streamTarget->appendStreamChunk(StreamSegment::Thinking, chunk);
		}
		else if (chunkType == QStringLiteral("text-delta") && !chunk.isEmpty()) {
			streamTarget->appendStreamChunk(StreamSegment::Reply, chunk);
		}
		result.kind = StreamFrameResult::Streaming;
		return result;
	}

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
			target->appendStreamChunk(thinking ? StreamSegment::Thinking : StreamSegment::Reply, chunk);
		}
		result.kind = StreamFrameResult::Streaming;
		return result;
	}

	// 工具调用/结果：没有 assistant/chunk 也要显示
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
		}
		return result;
	}

	// 其它类型不需更新气泡（user/message 发送时已上屏）
	return result;
}

MessageQuery* MessageQuery::fromEvents(const QJsonArray& events)
{
	// 与 HistoryLoader 共用 MessageQueryBuilder 管线；这里同步抽干全部批次后取结果
	MessageQueryBuilder builder;
	builder.start(events);
	while (builder.step()) {
	}
	return builder.takeResult();
}

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
	// 批量构建：先抑制逐次高度拟合，整批完成后统一拟合
	m_query->setBulkFitting(true);
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

	// 构建完成：关闭批量模式，对每个 Agent 气泡统一拟合一次
	m_query->setBulkFitting(false);
	for (MessageQuery::MessageUnit& message : m_query->messages) {
		if (message.agentUnit) {
			message.agentUnit->setBulkFit(false);
			message.agentUnit->updateHeightToContent();
		}
	}

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

HistoryLoader::HistoryLoader(DshApiClient* api, MessageQuery* messages, QVBoxLayout* layout,
	HistoryManager* history, QScrollArea* scrollArea, QObject* parent)
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
	// 日志事件的 seq，上翻分页拿它做 beforeSeq 游标
	int seqOf(const QJsonObject& event)
	{
		return event.value(QStringLiteral("seq")).toInt();
	}

	// 首屏快照播种的事件上限：全量构建会拖慢切会话，更早内容交给上翻分页
	constexpr int kSeedEventCap = 200;
}

void HistoryLoader::load(const QString& sessionId)
{
	if (sessionId.isEmpty())
		return;
	// 同会话已有请求在途时不重复发起；切会话则取消旧构建重新加载
	if (sessionId == m_sessionId && m_loading)
		return;

	if (sessionId != m_sessionId) {
		m_builder.cancel();
		m_reachedEnd = false;
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

	const int oldCount = m_history ? m_history->eventCount() : 0;
	const QString requestedSessionId = sessionId;

	if (!m_api || !m_history) {
		m_loading = false;
		return;
	}

	// session/page 必须带 follow 快照给的 throughSeq；游标未到先挂起，等 setStreamCursor() 唤醒
	if (m_throughSeq <= 0) {
		m_loadPending = true;
		qInfo().noquote() << "[History] load deferred until follow cursor sessionId=" << sessionId;

		// 看门狗：快照没来时不能一直转圈 —— 有回落游标就用它发请求，否则报错收 loading
		if (!m_cursorWatchdog) {
			m_cursorWatchdog = new QTimer(this);
			m_cursorWatchdog->setSingleShot(true);
			dshRegister(QStringLiteral("HistoryLoader.watchdog.%1").arg(reinterpret_cast<quintptr>(this)),
				m_cursorWatchdog, &QTimer::timeout, this, [this]() {
				if (!m_loadPending || m_sessionId.isEmpty())
					return;
				if (m_fallbackCursor > 0) {
					qWarning().noquote() << "[History] follow cursor timeout -> fallback throughSeq="
						<< m_fallbackCursor << " sessionId=" << m_sessionId;
					m_loadPending = false;
					m_throughSeq = m_fallbackCursor;
					m_seeded = true;
					load(m_sessionId);
					return;
				}

				qWarning().noquote() << "[History] follow cursor timeout, no fallback sessionId=" << m_sessionId;
				m_loadPending = false;
				m_loading = false;
				emit historyError(QStringLiteral("stream-timeout"),
					qtTrId("chat_history_timeout"));
			});
		}
		m_cursorWatchdog->start(2500);
		return;
	}

	QElapsedTimer requestTimer;
	requestTimer.start();

	// 上翻一页只取比 m_oldestSeq 更早的记录；不带 beforeSeq = 最近一页
	const int beforeSeq = (m_history->loadMoreRequested() && m_oldestSeq > 0) ? m_oldestSeq : 0;
	const QJsonObject args = SessionCommands::sessionPage(sessionId, m_throughSeq, m_history->limit(), beforeSeq);

	m_api->callMethod(QStringLiteral("session/page"), args,
		[this, generation, oldCount, requestedSessionId, requestTimer](const QJsonObject& value) {
			if (generation != m_loadGeneration)
				return;

			m_loading = false;
			if (requestedSessionId != m_sessionId)
				return;

			const QJsonArray events = SessionCommands::eventsFromRecords(
				value.value(QStringLiteral("records")).toArray());
			const bool serverHasMore = value.value(QStringLiteral("hasMore")).toBool();
			const int newCount = events.size();

			const bool paging = m_history->loadMoreRequested();

			// 头部插入会改变内容高度：先记锚点保偏移；用户本就在顶部(value≈0)时不校正
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
				// 到顶只看回包数量；用 hasMore 判定会误判（有更多却被当成没有更多）
				m_reachedEnd = !(newCount > oldCount);
			}

			qInfo().noquote() << "[History] events arrived sessionId=" << requestedSessionId
				<< "old=" << oldCount << "new=" << newCount
				<< "paging=" << paging << "oldestSeq=" << m_oldestSeq
				<< "roundTripMs=" << requestTimer.elapsed();

			if (paging) {
				// 带 beforeSeq 的请求返回的全是更早事件，整页插到顶部
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
						// 更早的一页先离屏分批构建，完成后整页插到顶部
						captureScrollAnchor();
						startPrepend(events);
					}
				}
			}
			else if (oldCount == 0) {
				// 此处不收遮罩，交给 continueBuild（内容真正上屏时），否则先露空白聊天区
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

			// 服务端确认没有更多且本次是点击请求：弹 toast，按钮保持原样
			if (m_history->loadMoreRequested() && !m_history->hasMore())
				emit noMoreHistory();
			m_history->setLoadMoreRequested(false);

			// 按钮显隐以 hasMore 为准；DSHHub 在列表非空时保持原样，避免整列重排
			if (!m_builder.isActive())
				emit loadMoreButtonVisibleChanged(m_history ? m_history->hasMore() : false);
		},
		[this, generation, requestedSessionId, requestTimer](const DshApiClient::RpcError& error) {
			if (generation != m_loadGeneration)
				return;

			m_loading = false;
			if (requestedSessionId != m_sessionId)
				return;
			qWarning().noquote() << "[History] session/page failed after"
				<< requestTimer.elapsed() << "ms sessionId=" << requestedSessionId
				<< "error=" << error.code << error.message;
			emit historyError(error.code, error.message);
		});
}

// 与 adoptSession() 分工：本函数把缓存建立时的游标/最早 seq/内容最新 seq 装回去，命中即秒开
void HistoryLoader::adoptCachedState(int throughSeq, int oldestSeq, int contentLastSeq, int eventCount, bool hasMore)
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

void HistoryLoader::setStreamCursor(int cursor)
{
	if (cursor <= 0)
		return;

	m_throughSeq = cursor;
	if (m_cursorWatchdog)
		m_cursorWatchdog->stop();
	if (m_loadPending && !m_sessionId.isEmpty()) {
		m_loadPending = false;
		load(m_sessionId);
	}
}

void HistoryLoader::setFallbackCursor(int cursor)
{
	m_fallbackCursor = cursor > 0 ? cursor : 0;
}

void HistoryLoader::seedFromPrefetched(const QString& sessionId, const QJsonArray& events, int throughSeq, bool hasMore)
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
		m_cursorWatchdog->stop();
	m_reachedEnd = !hasMore;

	qInfo().noquote() << "[History] seeded from prefetched page sessionId=" << sessionId
		<< "events=" << events.size() << "throughSeq=" << m_throughSeq
		<< "oldestSeq=" << m_oldestSeq << "hasMore=" << hasMore;

	// 遮罩不在此刻收（构建在隐藏容器里做）；内容真正上屏时由 continueBuild() 发
	emit loadMoreButtonVisibleChanged(hasMore);

	if (m_history) {
		m_history->setEventCount(events.size());
		m_history->setHasMore(hasMore);
	}

	startBuild(events);
}

void HistoryLoader::seedFromSnapshot(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore)
{
	if (sessionId.isEmpty() || sessionId != m_sessionId)
		return; // 切走后迟到的快照
	// 已有内容时只有快照更新才重建：cursor ≤ contentCut 说明期间无新事件，直接沿用
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
		m_cursorWatchdog->stop();
	m_seeded = true;
	m_loading = false;

	QJsonArray events = SessionCommands::eventsFromRecords(records);

	// 只播种最近 kSeedEventCap 条，更早的内容交给上翻分页
	bool truncated = false;
	if (events.size() > kSeedEventCap) {
		QJsonArray tail;
		for (int i = events.size() - kSeedEventCap; i < events.size(); ++i)
			tail.append(events.at(i));
		events = tail;
		truncated = true;
	}

	m_oldestSeq = events.isEmpty() ? 0 : seqOf(events.at(0).toObject());
	m_contentLastSeq = cursor > 0 ? cursor : m_oldestSeq;
	const bool effectiveHasMore = hasMore || truncated;

	qInfo().noquote() << "[History] seeded from follow snapshot sessionId=" << sessionId
		<< "cursor=" << m_throughSeq << "events=" << events.size()
		<< "oldestSeq=" << m_oldestSeq << "hasMore=" << effectiveHasMore
		<< "truncated=" << truncated;

	m_reachedEnd = !effectiveHasMore;
	emit loadMoreButtonVisibleChanged(effectiveHasMore);

	if (events.isEmpty() || !m_history) {
		// 无内容可建就不会有构建完成回调，遮罩必须在这里收
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
	// 上一次构建（首屏或离屏老页）未完成则忽略，避免多个离屏构建交错
	if (m_builder.isActive())
		return;

	// 只有服务端确认过没有更多才短路；其余一律真实请求，避免误判加载不出来
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
	// 按 seq 上翻：session/page 带 beforeSeq = 当前最早一条事件的 seq
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
	m_prependPending = prepend;
	m_buildSessionId = m_sessionId;
	m_buildGeneration = m_loadGeneration;
	m_builder.start(events);

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
		// 加载更多：把离屏构建好的老页一次性插到顶部
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

	// 内容此刻才真正上屏，到这里才通知 DSHHub 收初始化遮罩；提前发会先露空白聊天区
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

	// 气泡 refit 高度晚一步稳定，故分多次校正，保持锚点相对视口顶部的偏移
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
