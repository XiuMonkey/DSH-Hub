#include "MessageQuery.h"
#include "AgentMessageUnit.h"
#include "DshEventParser.h"
#include "DshApiClient.h"
#include "CacheHistoryManager.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QList>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QObject>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
	QPushButton* makeCopyButton(QWidget* widget)
	{
		auto* button = new QPushButton(QString(QChar(0x29C9))); // ⧉ 复制图标
		button->setToolTip(QStringLiteral("复制"));
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

UserMessageUnit* MessageQuery::insertUserMessage(const QString& text, QVBoxLayout* layout, int layoutIndex)
{
	auto* unit = new UserMessageUnit;
	unit->setMessage(text);

	auto* container = new QWidget;
	auto* box = new QVBoxLayout(container);
	box->setContentsMargins(0, 0, 0, 0);
	box->setSpacing(2);

	box->addWidget(unit, 0, Qt::AlignRight);

	box->addWidget(makeCopyButton(unit), 0, Qt::AlignRight);

	layout->insertWidget(layoutIndex, container, 0, Qt::AlignRight);
	messages.insert(messages.begin(), MessageUnit{ 0, nullptr, unit, nullptr, container });
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

AgentMessageUnit* MessageQuery::insertAgentMessage(const QString& markdown, QVBoxLayout* layout, int layoutIndex,
	const QString& thinking)
{
	auto* unit = new AgentMessageUnit;
	// AgentMessageUnit 外观见 resources/styles/chat.qss：#agentBubble 内的 #agentUnit 由下方容器规则覆盖为透明

	if (!thinking.isEmpty())
		unit->appendThinking(thinking);

	unit->appendMarkdownWithCodeShadow(markdown);

	auto* container = new QWidget;
	container->setObjectName(QStringLiteral("agentBubble"));
	container->setAttribute(Qt::WA_StyledBackground, true);
	// 气泡容器外观见 resources/styles/chat.qss（#agentBubble）
	auto* box = new QVBoxLayout(container);
	box->setContentsMargins(8, 8, 8, 8);
	box->setSpacing(2);

	box->addWidget(unit, 0, Qt::AlignLeft);

	box->addWidget(makeCopyButton(unit), 0, Qt::AlignLeft);

	layout->insertWidget(layoutIndex, container, 0, Qt::AlignLeft);
	messages.insert(messages.begin(), MessageUnit{ 1, unit, nullptr, nullptr, container });
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
	appendHistoryEventsToQuery(this, layout, events);
}

void MessageQuery::prependEvents(QVBoxLayout* layout, const QJsonArray& events, int layoutIndex)
{
	// 向前插入时，先把连续的 assistant/message 合并成同一个气泡，避免思考链被拆散；
	// tool/call 和 tool/result 会挂到当前 agent 气泡上，不再丢失。
	struct PendingTool
	{
		bool isResult = false;
		QString name;
		QString html;
	};

	struct PendingItem
	{
		int type = 0; // 0: user, 1: agent
		QString text;
		QString thinking;
		QList<PendingTool> tools;
	};

	QList<PendingItem> items;
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
				items.append({ 0, text, QString(), {} });
		}
		else if (type == QStringLiteral("assistant/message")) {
			const QString thinking = extractThinking(event);
			const QString reply = extractReply(event);
			if (reply.isEmpty() && thinking.isEmpty())
				continue;

			if (!items.isEmpty() && items.last().type == 1) {
				if (!items.last().text.isEmpty() && !reply.isEmpty())
					items.last().text += QStringLiteral("\n\n");
				items.last().text += reply;
				if (!thinking.isEmpty()) {
					if (!items.last().thinking.isEmpty())
						items.last().thinking += QStringLiteral("\n\n");
					items.last().thinking += thinking;
				}
			}
			else {
				items.append({ 1, reply, thinking, {} });
			}
		}
		else if (type == QStringLiteral("tool/call")) {
			const ToolCallInfo tool = extractToolCall(event);
			if (!tool.valid)
				continue;

			if (items.isEmpty() || items.last().type != 1)
				items.append({ 1, QString(), QString(), {} });

			const QString html = QStringLiteral("<pre style='white-space:pre-wrap;word-break:break-all;margin:0;'>%1</pre>")
				.arg(QString::fromUtf8(
					QJsonDocument(tool.arguments).toJson(QJsonDocument::Indented))
					.toHtmlEscaped());
			items.last().tools.append({ false, tool.name, html });
		}
		else if (type == QStringLiteral("tool/result")) {
			const ToolResultInfo result = extractToolResult(event);
			if (!result.valid)
				continue;

			if (items.isEmpty() || items.last().type != 1)
				items.append({ 1, QString(), QString(), {} });

			const QString html = QStringLiteral("<pre style='white-space:pre-wrap;word-break:break-all;margin:0;'>%1</pre>").arg(result.message.toHtmlEscaped());
			items.last().tools.append({ true, QString(), html });
		}
	}

	// 倒序插入，保证最终顺序仍然是 旧 -> 新
	for (int i = items.size() - 1; i >= 0; --i) {
		if (items.at(i).type == 0) {
			insertUserMessage(items.at(i).text, layout, layoutIndex);
		}
		else {
			AgentMessageUnit* unit = insertAgentMessage(items.at(i).text, layout, layoutIndex, items.at(i).thinking);
			for (const PendingTool& tool : items.at(i).tools) {
				if (tool.isResult)
					unit->appendToolResult(tool.html);
				else
					unit->appendToolCall(tool.name, tool.html);
			}
		}
	}
}

void MessageQuery::prependOlderEvents(QVBoxLayout* layout, const QJsonArray& events, int oldCount, int layoutIndex)
{
	QJsonArray older;
	const int newCount = events.size();
	for (int i = 0; i < newCount - oldCount; ++i)
		older.append(events.at(i));

	qInfo().noquote() << "[MessageQuery] prependOlderEvents add=" << older.size()
		<< "layoutIndex=" << layoutIndex
		<< "childrenBefore=" << (layout ? layout->count() : -1);

	prependEvents(layout, older, layoutIndex);
}

MessageQuery* MessageQuery::fromEvents(const QJsonArray& events)
{
	auto* holder = new QWidget;
	auto* layout = new QVBoxLayout(holder);

	auto* query = new MessageQuery;
	query->appendEvents(layout, events);

	// 从临时离屏布局中摘下，并解除父子关系，方便后续 attach 到真实消息区
	query->detachFromLayout(layout);
	query->releaseWidgets();
	delete holder;

	return query;
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
	m_events = events;
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

	// 构建完成：摘下控件并释放临时容器，m_query 留给 takeResult()
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

void HistoryLoader::load(const QString& sessionId)
{
	if (sessionId.isEmpty())
		return;

	// 同一个会话已有请求在途时不要重复发起；切换会话则允许取消旧构建并重新加载新会话。
	if (sessionId == m_sessionId && m_loading)
		return;

	if (sessionId != m_sessionId) {
		cancelBuild();
		m_usingPrefetched = false;
		m_reachedEnd = false; // 新会话重新判定
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

	QJsonObject payload;
	payload.insert(QStringLiteral("sessionId"), sessionId);
	payload.insert(QStringLiteral("maxMessages"), m_history->limit());

	m_api->callMethod(
		QStringLiteral("session.history"),
		payload,
		[this, generation, oldScroll, oldScrollMax, oldCount, requestedSessionId](const QJsonObject& value) {
			if (generation != m_loadGeneration)
				return;

			m_loading = false;
			emit loadingChanged(false);

			if (requestedSessionId != m_sessionId)
				return;

			const QJsonArray events = value.value(QStringLiteral("events")).toArray();
			const int newCount = events.size();

			// 到顶判定只用“服务端回包数量”：拉不到比已有更多的条数才算到顶。
			// 用 hasMore 判定在部分流程会误判（有更多却被当成没有更多）。
			if (newCount > oldCount)
				m_reachedEnd = false;
			else
				m_reachedEnd = true;

			qInfo().noquote() << "[History] events arrived sessionId=" << requestedSessionId
				<< "old=" << oldCount << "new=" << newCount
				<< "usingPrefetched=" << m_usingPrefetched;

			if (oldCount == 0) {
				m_history->setEventCount(newCount);
				m_history->setHasMore(newCount >= m_history->limit());
				emit loadMoreButtonVisibleChanged(false);

				// 从零打开的会话：像流式输出一样把历史分批直接铺到可见列表，
				// 不必等离屏整段构建完再整体换入（避免打开长会话时长时间空白）。
				if (m_messages && m_messages->messages.empty())
					startLiveAppend(events);
				else
					startBuild(events);
			}
			else if (newCount > oldCount) {
				m_history->setEventCount(newCount);
				m_history->setHasMore(newCount >= m_history->limit());

				if (m_usingPrefetched) {
					startBuild(events);
				}
				else {
					// 头部插入会改变内容高度。为避免“闪到底部/位置漂移”，
					// 用“视口顶部附近的某个已有消息”做锚点：记录插入前它相对
					// 视口顶部的偏移，插入后把该偏移保持住。由于气泡有延迟
					// refit（高度晚一步才稳定），分多次校正直到几何稳定。
					QScrollBar* bar = m_scrollArea ? m_scrollArea->verticalScrollBar() : nullptr;
					const int beforeVal = bar ? bar->value() : 0;

					// “加载更多”按钮位于列表顶部：若用户本来就在顶部（value≈0），
					// 新插入的老消息应当自然出现在视口顶部，Qt 会保持 value=0；
					// 此时做锚点校正反而会把视口往下推插入高度那么多
					// （看起来像“闪到底部”），所以顶部场景跳过全部校正。
					const bool userAtTop = bar && beforeVal <= 8;

					QPointer<QWidget> anchor;
					int anchorTopBefore = 0;
					if (!userAtTop && bar && m_scrollArea->widget() && m_layout) {
						QWidget* content = m_scrollArea->widget();
						for (int i = 0; i < m_layout->count(); ++i) {
							QLayoutItem* item = m_layout->itemAt(i);
							QWidget* w = item ? item->widget() : nullptr;
							if (!w || !w->isVisible())
								continue;
							const int top = w->geometry().top();
							const int bottom = top + w->geometry().height();
							if (bottom > beforeVal) {
								anchor = w;
								anchorTopBefore = top;
								break;
							}
						}
						if (anchor.isNull() && m_layout->count() > 0) {
							if (QLayoutItem* item = m_layout->itemAt(m_layout->count() - 1)) {
								if (QWidget* w = item->widget()) {
									anchor = w;
									anchorTopBefore = w->geometry().top();
								}
							}
						}
						Q_UNUSED(content)
					}

					if (m_messages && m_layout)
						m_messages->prependOlderEvents(m_layout, events, oldCount, 1);

					if (!userAtTop && m_scrollArea && !anchor.isNull()) {
						const int keep = anchorTopBefore - beforeVal;
						const auto applyAnchor = [this, anchor, keep]() {
							if (!m_scrollArea)
								return;
							QScrollBar* scrollBar = m_scrollArea->verticalScrollBar();
							if (!scrollBar || anchor.isNull())
								return;
							const int newTop = anchor->geometry().top();
							scrollBar->setValue(qBound(0, newTop - keep, scrollBar->maximum()));
						};
						QTimer::singleShot(0, this, applyAnchor);
						QTimer::singleShot(40, this, applyAnchor);
						QTimer::singleShot(120, this, applyAnchor);
					}
				}
			}
			else {
				m_history->setHasMore(false);
			}

			// 服务端确认没有更多：弹 toast（若本次点击请求过）；按钮仍保持原样
			if (m_history->loadMoreRequested() && !m_history->hasMore())
				emit noMoreHistory();
			m_history->setLoadMoreRequested(false);

			// 按钮显隐以 hasMore 为准；DSHHub 侧在列表非空时会保持按钮原样，
			// 不因 hide/toast 造成整列重排。
			if (!m_builder.isActive() && !m_liveActive)
				emit loadMoreButtonVisibleChanged(m_history ? m_history->hasMore() : false);
		},
		[this, generation, requestedSessionId](const DshApiClient::RpcError& error) {
			if (generation != m_loadGeneration)
				return;

			m_loading = false;
			m_usingPrefetched = false;
			emit loadingChanged(false);
			if (requestedSessionId != m_sessionId)
				return;
			emit historyError(error.code, error.message);
		});
}

void HistoryLoader::loadMore()
{
	if (m_sessionId.isEmpty() || m_loading)
		return;
	if (!m_history)
		return;

	// 正在“可视增量”铺历史（会话刚打开），先别叠加加载更多请求
	if (m_liveActive)
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
	m_history->increaseLimit(20);
	load(m_sessionId);
}

void HistoryLoader::setUsingPrefetched(bool usingPrefetched)
{
	m_usingPrefetched = usingPrefetched;
}

void HistoryLoader::setMessages(MessageQuery* messages)
{
	m_messages = messages;
}

void HistoryLoader::cancelBuild()
{
	m_liveActive = false;
	m_liveEvents = QJsonArray();
	m_liveIndex = 0;
	m_builder.cancel();
}

void HistoryLoader::startLiveAppend(const QJsonArray& events)
{
	m_liveActive = true;
	m_liveEvents = events;
	m_liveIndex = 0;
	m_liveGeneration = m_loadGeneration;
	// 开始时在底部（空列表/最新消息处）才自动贴底；用户一旦上翻就停止跟随
	QScrollBar* bar = m_scrollArea ? m_scrollArea->verticalScrollBar() : nullptr;
	m_liveFollow = bar && (bar->value() >= bar->maximum() - 24);
	QTimer::singleShot(0, this, [this]() { liveAppendStep(); });
}

void HistoryLoader::liveAppendStep()
{
	if (!m_liveActive)
		return;

	// 期间会话被切换/重新加载则放弃本次增量
	if (m_liveGeneration != m_loadGeneration) {
		cancelBuild();
		return;
	}

	constexpr int batchSize = 8;
	const int end = qMin(m_liveIndex + batchSize, m_liveEvents.size());
	if (m_liveIndex < end && m_messages && m_layout) {
		QJsonArray batch;
		for (int i = m_liveIndex; i < end; ++i)
			batch.append(m_liveEvents.at(i));
		m_liveIndex = end;
		m_messages->appendEvents(m_layout, batch);

		// 像流式输出一样贴底：用户接近底部才继续跟随，上翻则停止，
		// 避免增量期间反复把视口拽回去。
		if (m_scrollArea && m_scrollArea->verticalScrollBar()) {
			QScrollBar* bar = m_scrollArea->verticalScrollBar();
			if (m_liveFollow) {
				if (bar->value() < bar->maximum() - 24)
					m_liveFollow = false; // 用户上翻，停止自动贴底
				else
					QTimer::singleShot(0, this, [this]() {
						if (m_scrollArea && m_scrollArea->verticalScrollBar())
							m_scrollArea->verticalScrollBar()->setValue(
								m_scrollArea->verticalScrollBar()->maximum());
					});
			}
		}
	}

	if (m_liveIndex >= m_liveEvents.size()) {
		const bool hasMore = m_history ? m_history->hasMore() : false;
		cancelBuild();
		emit loadMoreButtonVisibleChanged(hasMore);
		return;
	}

	QTimer::singleShot(0, this, [this]() { liveAppendStep(); });
}

void HistoryLoader::startBuild(const QJsonArray& events)
{
	m_buildSessionId = m_sessionId;
	m_buildGeneration = m_loadGeneration;
	m_builder.start(events);
	continueBuild();
}

void HistoryLoader::continueBuild()
{
	if (!m_builder.isActive())
		return;

	if (m_buildGeneration != m_loadGeneration) {
		m_builder.cancel();
		return;
	}

	if (m_builder.step()) {
		QTimer::singleShot(0, this, [this]() { continueBuild(); });
		return;
	}

	MessageQuery* query = m_builder.takeResult();
	if (m_buildSessionId != m_sessionId || m_buildGeneration != m_loadGeneration) {
		delete query;
		return;
	}

	emit incrementalBuildReady(query);
	emit loadMoreButtonVisibleChanged(true);
}