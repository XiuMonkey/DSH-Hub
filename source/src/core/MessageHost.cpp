// 见 MessageHost.h 的分工说明。交互面板与 mux 帧路由（原 DSHHub::handleMuxFrame）整体下沉到这里，
// 面板的"建"与"收"因此归同一个类；窗口侧只需接 contentReady / contentReplaced / turnFinished。

#include "core/MessageHost.h"

#include "ui/ChatInputWidget.h"
#include "network/DshApiClient.h"
#include "common/appearance/InteractionHandler.h"
#include "ui/LoadMoreButton.h"
#include "chat/MessageQuery.h"
#include "common/session/SessionCommands.h"
#include "ui/SmoothWheelScroller.h"
#include "ui/SpinnerWidget.h"
#include "core/ConnectionManager.h"
#include "common/util/Logger.h"

#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

MessageHost::MessageHost(DshApiClient* api, CacheManager* cache, QScrollArea* scrollArea,
	QVBoxLayout* messagesLayout, LoadMoreButton* loadMoreButton, QLabel* toastLabel,
	ChatInputWidget* chatInput, QObject* parent)
	: QObject(parent)
	, m_cache(cache)
	, m_api(api)
	, m_scrollArea(scrollArea)
	, m_layout(messagesLayout)
	, m_loadMoreButton(loadMoreButton)
	, m_toastLabel(toastLabel)
	, m_chatInput(chatInput)
{
	// 流式渲染节流：避免每个 chunk 都全量重渲染导致卡顿
	m_streamTimer = new QTimer(this);
	m_streamTimer->setInterval(50);
	m_streamTimer->setSingleShot(true);
	dshRegister("MessageHost.001", m_streamTimer, &QTimer::timeout, this, &MessageHost::flushStreamingFrame);

	// 滚轮平滑：Qt 默认一格滚轮是一次同步 setValue（实测 60px 无过渡帧），观感是跳格。
	// 这里接管聊天区视口的滚轮做补间，并借 isAnimating() 认出"用户正在滚"。
	if (m_scrollArea) {
		m_wheelScroller = new SmoothWheelScroller(m_scrollArea);
		// 用户往上滚 = 想看更早的内容：立刻关掉跟随。该信号同步发出，此刻滚动条通常还贴着
		// 底部（补间还没走第一步），所以必须靠信号而不是位置。
		dshRegister("MessageHost.002", m_wheelScroller, &SmoothWheelScroller::userScrolledAway, this,
			[this]() {
				m_followBottom = false;
			});

		// 反过来：位置真的到了底部（滚回来、拖到底、或被我们钉到底）就自动恢复跟随
		if (QScrollBar* bar = m_scrollArea->verticalScrollBar()) {
			dshRegister("MessageHost.003", bar, qOverload<int>(&QScrollBar::valueChanged), this,
				[this](int value) {
					QScrollBar* current = m_scrollArea ? m_scrollArea->verticalScrollBar() : nullptr;
					if (current && value >= current->maximum())
						m_followBottom = true;
				});
		}
	}

	// 输入区的发送/中止随输入区一起归这里管
	if (m_chatInput) {
		dshRegister("MessageHost.004", m_chatInput, &ChatInputWidget::sendRequested, this,
			&MessageHost::onSendClicked);
		dshRegister("MessageHost.005", m_chatInput, &ChatInputWidget::stopRequested, this,
			&MessageHost::onStopRequested);
	}

	// 一上来就持有一个空列表：这样所有"往当前列表写东西"的调用点都不必再判空
	m_messages = new MessageQuery;
	m_history.setLimit(20);

	m_loader = new HistoryLoader(api, m_messages, m_layout, &m_history, m_scrollArea, this);

	// loader 的信号全部在这里落地——DSHHub 不再需要知道分页与构建的细节
	dshRegister("MessageHost.006", m_loader, &HistoryLoader::firstHistoryArrived, this, [this]() {
		emit contentReady();
	});
	dshRegister("MessageHost.007", m_loader, &HistoryLoader::loadMoreButtonVisibleChanged, this,
		&MessageHost::setLoadMoreVisible);
	dshRegister("MessageHost.008", m_loader, &HistoryLoader::historyError, this,
		[this](const QString& code, const QString& message) {
			addSystemMessage(QStringLiteral("History error: %1 %2").arg(code, message));
			// 出错也要收掉"载入中"提示：内容不会再来了，不能一直盖着
			hideLoading();
		});
	dshRegister("MessageHost.009", m_loader, &HistoryLoader::incrementalBuildReady, this,
		&MessageHost::swapInBuilt);
	dshRegister("MessageHost.010", m_loader, &HistoryLoader::noMoreHistory, this, [this]() {
		// 弹“没有更多了”toast；按钮保持原样不隐藏不改文案，避免 hide/显隐造成整列消息重排重绘
		showNoMoreToast();
	});
	if (m_loadMoreButton)
		dshRegister("MessageHost.011", m_loadMoreButton, qOverload<bool>(&QPushButton::clicked),
			m_loader, &HistoryLoader::loadMore);
}

MessageHost::~MessageHost()
{
	// 只管这个壳对象：列表里的控件随父窗口析构
	delete m_messages;
	m_messages = nullptr;
}

void MessageHost::showSession(const QString& sessionId, int fallbackCursor, int observedLastSeq)
{
	// 上一个会话尚未完成的增量构建先取消，避免旧消息覆盖新会话
	cancelBuild();

	// 手上的内容交缓存（元数据里的"内容最新位置"用 DSHHub 观测到的 seq），随后换成空实例
	handOffToCache(observedLastSeq);

	m_sessionId = sessionId;
	// follow 快照没来时的回落游标：session/list 行的 projections.asOfSeq
	if (m_loader)
		m_loader->setFallbackCursor(fallbackCursor);

	// 从这里到"新内容上屏"之间聊天区是空的（旧内容已在上面摘掉）：分批构建跨多个事件循环
	// 轮次，窗口会把空聊天区画出来，所以亮提示层。缓存命中那条路是同一轮内亮完又收，不会闪。
	showLoading();

	if (restoreFromCache(sessionId))
		return;

	// 控件缓存未命中，但预取可能已经有这一页：先用它点亮（0 网络等待），
	// 随后 follow 快照会因"不比已有内容新"而不重建。
	CacheManager::PrefetchedHistory prefetched;
	if (m_cache && m_cache->takePrefetchedHistory(sessionId, &prefetched)
		&& !prefetched.events.isEmpty()) {
		TimingLogger::mark(QStringLiteral("prefetch hit -> paint without waiting for follow"));
		showPrefetched(sessionId, prefetched.events, prefetched.throughSeq, prefetched.hasMore);
		return;
	}

	TimingLogger::mark(QStringLiteral("cache miss -> history fetch begin"));
	startFreshFetch(sessionId);
}

void MessageHost::showFreshSession(const QString& sessionId, bool loadHistory, int fallbackCursor, int observedLastSeq)
{
	cancelBuild();
	// 旧会话的内容照样交缓存：切到"刚创建的空会话"时也要把上一个会话存好
	handOffToCache(observedLastSeq);

	m_sessionId = sessionId;
	// follow 快照没来时的回落游标：session/list 行的 projections.asOfSeq
	if (m_loader)
		m_loader->setFallbackCursor(fallbackCursor);
	m_history.reset();
	if (m_loadMoreButton)
		m_loadMoreButton->hide();

	if (m_loader) {
		m_loader->setMessages(m_messages);
		if (loadHistory)
			m_loader->load(sessionId);
		else
			m_loader->adoptSession(sessionId);
	}
}

void MessageHost::cancelBuild()
{
	if (m_loader)
		m_loader->cancelBuild();
}

void MessageHost::setStreamCursor(int cursor)
{
	if (m_loader)
		m_loader->setStreamCursor(cursor);
}

void MessageHost::onFollowSnapshot(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore)
{
	if (m_loader)
		m_loader->seedFromSnapshot(sessionId, cursor, records, hasMore);
}

void MessageHost::discardCurrent()
{
	// 空会话 id = 丢弃（当前会话被删除，内容没必要留在缓存里）
	if (m_cache)
		m_cache->cacheOrDiscardCurrentSession(QString(), m_messages, m_layout);

	m_messages = new MessageQuery;
	m_sessionId.clear();
	m_history.reset();
	if (m_loadMoreButton)
		m_loadMoreButton->hide();
}

void MessageHost::clearCurrent()
{
	if (m_messages)
		m_messages->clear();
}

void MessageHost::handOffToCache(int observedLastSeq)
{
	const QString cachedSessionId = m_sessionId;
	if (m_cache)
		m_cache->cacheOrDiscardCurrentSession(m_sessionId, m_messages, m_layout);

	// 分页状态（原始事件数 / 是否有更早内容 / 游标）一并快照进缓存，恢复时可跳过重拉与二次渲染
	if (!cachedSessionId.isEmpty() && m_cache) {
		const int cursor = m_loader ? m_loader->streamCursor() : 0;
		const int oldest = m_loader ? m_loader->oldestSeq() : 0;
		m_cache->storeCacheMeta(cachedSessionId, m_history.eventCount(), m_history.hasMore(),
			cursor, oldest,
			// 缓存内容的最新位置：游标之后还可能有实时事件，取两者较大者
			qMax(cursor, observedLastSeq));
	}

	// 交出去之后立刻换上一个空实例：current() 因此永远非空。
	m_messages = new MessageQuery;
}

bool MessageHost::restoreFromCache(const QString& sessionId)
{
	if (!m_cache)
		return false;

	int cachedRawCount = 0;
	bool cachedHasMore = false;
	int cachedThroughSeq = 0;
	int cachedOldestSeq = 0;
	int cachedLastSeq = 0;
	// 快照必须先于 restore 取出：restoreCachedSession(take) 会清掉缓存快照
	const bool haveMeta = m_cache->takeCacheMeta(sessionId, &cachedRawCount, &cachedHasMore,
		&cachedThroughSeq, &cachedOldestSeq, &cachedLastSeq);
	MessageQuery* restored = m_cache->restoreCachedSession(sessionId, m_layout);
	if (!restored)
		return false;

	TimingLogger::mark(QStringLiteral("cache hit -> instant restore"));

	// 让位给恢复的实例：空壳（无控件）直接删；异常路径下若还带内容则交缓存丢弃，
	// 别让两棵树同时挂在布局上
	if (m_messages && m_messages->messages.empty())
		delete m_messages;
	else if (m_messages)
		m_cache->cacheOrDiscardCurrentSession(QString(), m_messages, m_layout);
	m_messages = restored;

	if (m_loadMoreButton)
		m_loadMoreButton->setVisible(!m_messages->messages.empty());

	// 恢复后把当前列表同步给 loader，避免它停留在上一个（可能已被删除/接管）会话的对象上
	if (m_loader)
		m_loader->setMessages(m_messages);

	// 窗口侧收尾：收交互面板 + 滚到底（原来在 swapToMessageQuery 里做的）
	emit contentReplaced();

	// 缓存是否过期由"缓存记录的内容位置 vs 随后到达的 follow 快照 cursor"判定
	// （见 adoptCachedState / seedFromSnapshot）：快照不比缓存新则只恢复分页状态；
	// 快照更新则 loader 自己重建，这里不提前重拉。
	if (!haveMeta) {
		// 兜底（无分页元数据的缓存）：按可见内容估算，不显示"加载更多"
		cachedRawCount = static_cast<int>(m_messages->messages.size());
		cachedHasMore = false;
	}
	TimingLogger::mark(QStringLiteral("cache restore -> seed pagination (rawCount=%1 hasMore=%2)")
		.arg(cachedRawCount)
		.arg(cachedHasMore ? QStringLiteral("true") : QStringLiteral("false")));

	m_history.reset();
	m_history.setEventCount(cachedRawCount);
	m_history.setHasMore(cachedHasMore);
	if (m_loader) {
		m_loader->cancelBuild();
		m_loader->adoptSession(sessionId);
		// 装回缓存建立时的游标 / 最早 seq / 内容最新 seq，供随后的 follow 快照判断缓存是否最新
		m_loader->adoptCachedState(cachedThroughSeq, cachedOldestSeq, cachedLastSeq, cachedRawCount, cachedHasMore);
	}
	setLoadMoreVisible(cachedHasMore);

	// 内容此刻已在实时布局里可见：启动期首屏（遮罩还在）在这里收掉——这条缓存命中路径
	// 没有别的地方会收遮罩（finishInitialization 幂等）
	emit contentReady();
	hideLoading();

	return true;
}

void MessageHost::startFreshFetch(const QString& sessionId)
{
	// 手上那个空实例继续用（handOffToCache 刚换上来的）
	m_history.setLimit(20);
	m_history.setHasMore(false);
	// 0.1.5：首屏由 session/follow 快照播种，这里只清掉上一会话的计数
	m_history.setEventCount(0);

	if (m_loadMoreButton)
		m_loadMoreButton->hide();

	if (m_loader) {
		m_loader->setMessages(m_messages);
		m_loader->load(sessionId);
	}
}

void MessageHost::swapInBuilt(MessageQuery* query)
{
	if (!query)
		return;

	if (m_scrollArea)
		m_scrollArea->setUpdatesEnabled(false);

	if (m_messages) {
		m_messages->clear();
		delete m_messages;
		m_messages = nullptr;
	}

	m_messages = query;
	m_messages->attachToLayout(m_layout);

	// 整表替换（旧对象已 delete）后必须同步 loader，否则它仍持有指向已释放 MessageQuery 的悬垂
	// 指针，加载更多/历史回调会在 prepend/insert 时崩溃（访问已释放的 messages 容器）
	if (m_loader)
		m_loader->setMessages(m_messages);

	// 窗口侧收尾：收掉交互面板 + 在刷新前同步滚到底（避免先显示顶部再闪烁）
	emit contentReplaced();

	// 内容已上屏：收掉"正在载入"提示（分批构建路径的收尾）
	hideLoading();
}

MessageHost::PrefetchOutcome MessageHost::onPrefetched(const QString& sessionId, const QJsonArray& events,
	int throughSeq, bool hasMore)
{
	if (sessionId.isEmpty() || events.isEmpty() || !m_cache)
		return PrefetchOutcome::Ignored;

	CacheManager::PrefetchedHistory prefetched;
	prefetched.events = events;
	prefetched.throughSeq = throughSeq;
	prefetched.hasMore = hasMore;
	m_cache->storePrefetchedHistory(sessionId, prefetched);

	if (sessionId == m_sessionId && m_messages && m_messages->messages.empty()) {
		showPrefetched(sessionId, events, throughSeq, hasMore);
		return PrefetchOutcome::Painted;
	}

	if (m_cache->hasCachedMessages(sessionId))
		return PrefetchOutcome::Ignored;

	// 只有小页才值得预构建控件树：小页能在几毫秒内挂完（实测 ~3ms），大页预构建反而更慢
	// ——一次性造树 + 首次挂载要付整棵树的冷布局/首绘（116 条实测挂载 307ms、预热 377ms），
	// 而"预取事件 + 分批构建"只要 4ms 起建、~145ms 铺完且不卡帧
	static constexpr int kPrebuildEventCap = 40;
	if (events.size() > kPrebuildEventCap)
		return PrefetchOutcome::Ignored;

	enqueuePrebuild(sessionId);
	return PrefetchOutcome::Queued;
}

// 用预取的一页事件立即点亮当前会话（走"路 1"：分批构建，不贴预构建的控件树）。
// 一次性贴树会在单帧里做完 addWidget + show + 整棵树的冷布局/首绘（116 条实测 ≈307ms 硬卡顿），
// 分批构建则 4ms 起建、~145ms 铺完、单帧 ≤6ms。滚到底与收遮罩都留到构建完成时，避免先露空白。
void MessageHost::showPrefetched(const QString& sessionId, const QJsonArray& events, int throughSeq, bool hasMore)
{
	if (sessionId != m_sessionId || events.isEmpty())
		return;

	TimingLogger::mark(QStringLiteral("prefetched history paint begin (%1 events)").arg(events.size()));

	if (m_messages && m_messages->messages.empty())
		delete m_messages;
	m_messages = new MessageQuery;
	m_history.setLimit(20);
	m_history.setHasMore(hasMore);
	m_history.setEventCount(events.size());

	if (m_loadMoreButton)
		m_loadMoreButton->hide();

	if (m_loader) {
		m_loader->setMessages(m_messages);
		m_loader->cancelBuild();
		m_loader->adoptSession(sessionId);
		m_loader->seedFromPrefetched(sessionId, events, throughSeq, hasMore);
	}

	setLoadMoreVisible(hasMore);

	TimingLogger::mark(QStringLiteral("prefetched history seeded (%1 events, incremental build)").arg(events.size()));
}

/** 把某会话排入预构建队列（配额有限，且每轮事件循环只建一个）。 */
void MessageHost::enqueuePrebuild(const QString& sessionId)
{
	if (m_prebuildRemaining <= 0 || sessionId.isEmpty())
		return;
	if (m_prebuildQueue.contains(sessionId))
		return;

	m_prebuildQueue.append(sessionId);
	--m_prebuildRemaining;
	QTimer::singleShot(0, this, &MessageHost::processPrebuildQueue);
}

/** 消费预构建队列：把预取事件造成控件树塞进缓存（进入该会话即 cache hit）。 */
void MessageHost::processPrebuildQueue()
{
	if (m_prebuilding || m_prebuildQueue.isEmpty())
		return;

	m_prebuilding = true;

	const QString sessionId = m_prebuildQueue.takeFirst();
	CacheManager::PrefetchedHistory prefetched;
	if (m_cache
		&& !m_cache->hasCachedMessages(sessionId)
		&& m_cache->takePrefetchedHistory(sessionId, &prefetched)
		&& !prefetched.events.isEmpty()) {
		// 只造树、不预热。曾试过"预热"（按真实列宽算尺寸 + ensurePolished + render）：116 条要
		// 377ms（render 占 268ms），却只把点击后的恢复从 307ms 降到 246ms——真正的开销在"挂进
		// 实时布局后的窗口绘制"，render 到 pixmap 换不来，而那 377ms 会变成启动期一帧硬卡顿。
		MessageQuery* query = MessageQuery::fromEvents(prefetched.events);

		m_cache->cacheSessionMessages(sessionId, query);

		// 分页状态与控件树一起记下：恢复时可跳过重拉与重建。
		// 必须在 cacheSessionMessages 之后写——它会清掉该会话的旧元数据。
		const int firstSeq = prefetched.events.first().toObject()
			.value(QStringLiteral("seq")).toInt();
		m_cache->storeCacheMeta(sessionId, prefetched.events.size(), prefetched.hasMore,
			prefetched.throughSeq, firstSeq, prefetched.throughSeq);

		qInfo().noquote() << "[MessageHost] prebuilt cached session id=" << sessionId
			<< "events=" << prefetched.events.size();
	}

	m_prebuilding = false;

	if (!m_prebuildQueue.isEmpty())
		QTimer::singleShot(0, this, &MessageHost::processPrebuildQueue);
}

void MessageHost::addSystemMessage(const QString& text)
{
	if (m_messages)
		m_messages->addSystemMessage(text, m_layout);
}

/** 加载更多按钮的显隐（loader 的信号驱动，也用于缓存恢复后同步一次）。 */
void MessageHost::setLoadMoreVisible(bool visible)
{
	if (!m_loadMoreButton)
		return;

	if (visible) {
		m_loadMoreButton->show();
		m_loadMoreButton->setEnabled(true);
		if (m_loadMoreButton->text() != qtTrId("common_load_more"))
			m_loadMoreButton->setText(qtTrId("common_load_more"));
	}
	else {
		m_loadMoreButton->hide();
	}
}

void MessageHost::showNoMoreToast()
{
	if (!m_toastLabel)
		return;

	QWidget* parent = qobject_cast<QWidget*>(m_toastLabel->parent());
	if (!parent)
		parent = m_scrollArea;

	m_toastLabel->setText(qtTrId("common_no_more_items"));
	m_toastLabel->adjustSize();
	m_toastLabel->setGeometry((parent->width() - m_toastLabel->width() - 32) / 2,
		parent->height() - m_toastLabel->height() - 24, m_toastLabel->width() + 32, m_toastLabel->height());
	m_toastLabel->show();
	m_toastLabel->raise();

	auto* effect = new QGraphicsOpacityEffect(m_toastLabel);
	m_toastLabel->setGraphicsEffect(effect);

	auto* fadeIn = new QPropertyAnimation(effect, "opacity", m_toastLabel);
	fadeIn->setDuration(180);
	fadeIn->setStartValue(0.0);
	fadeIn->setEndValue(1.0);
	// 动画对象随播放自毁，不进登记表
	connect(fadeIn, &QPropertyAnimation::finished, this, [this]() {
		QTimer::singleShot(1200, this, [this]() {
			if (!m_toastLabel)
				return;

			auto* effect = qobject_cast<QGraphicsOpacityEffect*>(m_toastLabel->graphicsEffect());
			if (!effect)
				return;

			auto* fadeOut = new QPropertyAnimation(effect, "opacity", m_toastLabel);
			fadeOut->setDuration(300);
			fadeOut->setStartValue(1.0);
			fadeOut->setEndValue(0.0);
			connect(fadeOut, &QPropertyAnimation::finished, m_toastLabel, [this]() {
				if (m_toastLabel)
					m_toastLabel->hide();
				});
			fadeOut->start(QAbstractAnimation::DeleteWhenStopped);
		});
	});
	fadeIn->start(QAbstractAnimation::DeleteWhenStopped);
}

/**
 * 会话切换期间的"载入中"提示（只盖聊天区：不动侧边栏、输入框、滚动条）。
 *
 * 切会话时旧内容被立刻摘走交缓存，新内容要等"预取 + 分批构建"完成（跨多个事件循环轮次，实测
 * 大页 ~146ms；未命中预取还要先等一次 session/page 往返），中间窗口会把空聊天区画出来，观感
 * 就是"纯白 + 干等"。同步挂载的缓存命中路径没有这个问题：摘旧的与挂新的在同一轮里完成。
 *
 * 不铺蒙版、不加卡片，只有居中的小转圈 + 一行"正在载入会话..."，整层鼠标穿透——只提供"在加载"
 * 的心理反馈，不遮挡也不拦截操作。幂等：重复调用只刷新几何与看门狗，看门狗兜底 6 秒。
 */
void MessageHost::showLoading()
{
	if (!m_scrollArea)
		return;

	if (!m_loadingOverlay) {
		QWidget* viewport = m_scrollArea->viewport();
		m_loadingOverlay = new QWidget(viewport);
		m_loadingOverlay->setObjectName(QStringLiteral("sessionLoadingOverlay"));
		m_loadingOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);

		auto* overlayLayout = new QVBoxLayout(m_loadingOverlay);
		overlayLayout->setContentsMargins(0, 0, 0, 0);

		auto* row = new QWidget(m_loadingOverlay);
		row->setAttribute(Qt::WA_TransparentForMouseEvents, true);

		auto* rowLayout = new QHBoxLayout(row);
		rowLayout->setContentsMargins(0, 0, 0, 0);
		rowLayout->setSpacing(10);

		auto* spinner = new SpinnerWidget(row);
		spinner->setFixedSize(20, 20);
		spinner->setAttribute(Qt::WA_TransparentForMouseEvents, true);
		spinner->start();
		rowLayout->addWidget(spinner, 0, Qt::AlignVCenter);

		m_loadingLabel = new QLabel(qtTrId("session_loading"), row);
		m_loadingLabel->setObjectName(QStringLiteral("sessionLoadingLabel"));
		m_loadingLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
		m_loadingLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
		rowLayout->addWidget(m_loadingLabel, 0, Qt::AlignVCenter);

		overlayLayout->addWidget(row, 0, Qt::AlignCenter);

		m_loadingWatchdog = new QTimer(this);
		m_loadingWatchdog->setSingleShot(true);
		dshRegister("MessageHost.012", m_loadingWatchdog, &QTimer::timeout, this,
			[this]() {
				qWarning().noquote() << "[MessageHost] loading overlay watchdog fired (a hide was missed?)";
				hideLoading();
			});
	}

	m_loadingOverlay->setGeometry(m_scrollArea->viewport()->rect());
	m_loadingOverlay->raise();
	m_loadingOverlay->show();
	m_loadingWatchdog->start(6000);
	TimingLogger::mark(QStringLiteral("session loading overlay shown"));
}

void MessageHost::hideLoading()
{
	if (m_loadingWatchdog)
		m_loadingWatchdog->stop();
	if (!m_loadingOverlay || !m_loadingOverlay->isVisible())
		return;

	m_loadingOverlay->hide();
	TimingLogger::mark(QStringLiteral("session loading overlay hidden"));
}

void MessageHost::syncLoadingGeometry()
{
	if (m_loadingOverlay && m_scrollArea)
		m_loadingOverlay->setGeometry(m_scrollArea->viewport()->rect());
}

void MessageHost::onStopRequested()
{
	if (!m_streaming)
		return;

	if (!m_api || m_sessionId.isEmpty()) {
		// 没有可用会话时也把本地流式状态复位，避免按钮卡在中止态
		m_streaming = false;
		updateStreamingUi();
		return;
	}

	m_api->callMethod(QStringLiteral("session/cancel"), SessionCommands::sessionCancel(m_sessionId),
		[this](const QJsonObject&) {
			m_streaming = false;
			if (m_streamTimer)
				m_streamTimer->stop();
			if (current() && current()->lastAgentUnitIfLast())
				current()->lastAgentUnitIfLast()->flushStream();
			updateStreamingUi();
		},
		[this](const DshApiClient::RpcError& error) {
			// cancel 失败也恢复按钮状态，避免 UI 一直卡在中止态
			m_streaming = false;
			if (m_streamTimer)
				m_streamTimer->stop();
			if (current() && current()->lastAgentUnitIfLast())
				current()->lastAgentUnitIfLast()->flushStream();
			updateStreamingUi();
			if (current()) {
				addSystemMessage(qtTrId("chat_abort_failed_fmt").arg(error.code, error.message));
			}
		});
}

void MessageHost::updateStreamingUi()
{
	if (m_chatInput)
		m_chatInput->setStreaming(m_streaming);
}

void MessageHost::sendPrompt(const QString& text)
{
	if (m_sessionId.isEmpty() || !current())
		return;

	m_streaming = false;
	if (m_streamTimer)
		m_streamTimer->stop();
	updateStreamingUi();
	current()->addUserMessage(text, m_layout);

	m_api->callMethod(QStringLiteral("session/prompt"), SessionCommands::sessionPrompt(m_sessionId, text),
		[this](const QJsonObject&) {
		},
		[this](const DshApiClient::RpcError& error) {
			if (current()) {
				addSystemMessage(qtTrId("chat_send_failed_fmt").arg(error.code, error.message));
			}
		});

	m_chatInput->clear();
}

void MessageHost::scrollToBottomNow()
{
	if (!m_scrollArea || !m_scrollArea->widget())
		return;

	// 平滑滚轮的补间会持续改 value，而这里要把位置直接钉到底部：先停掉它，否则两者互相打架
	// （表现为刚滚到底又被补间的旧目标拉回去一截）。
	if (m_wheelScroller)
		m_wheelScroller->stop();

	// 显式滚到底 = 恢复跟随（之后的新内容继续自动跟随）
	m_followBottom = true;

	// 避免流式输出时频繁触发多个滚动任务
	if (m_scrollToBottomScheduled)
		return;
	m_scrollToBottomScheduled = true;

	// 在布局完成前一直禁用刷新，等滚动到底部后再一次性显示，避免先看到顶部再闪到底部。
	m_scrollArea->setUpdatesEnabled(false);

	fitContentThenLayout();

	// 等 Qt 完成本轮布局/事件处理后，再真正滚动并恢复刷新
	QTimer::singleShot(0, this, [this]() {
		if (!m_scrollArea)
			return;

		fitContentThenLayout();

		if (m_scrollArea->verticalScrollBar())
			m_scrollArea->verticalScrollBar()->setValue(m_scrollArea->verticalScrollBar()->maximum());

		m_scrollToBottomScheduled = false;
		m_scrollArea->setUpdatesEnabled(true);
		m_scrollArea->viewport()->update();
		});
}

// 流式渲染的一帧：本帧先暂停重绘，等拟合/布局落定后再一次性画出来。
// flushStream() 每帧都会重建 live 区域，新视图刚创建时宽高还是 Qt 默认值、布局也还没铺完，
// 这些中间态一旦被画到屏幕上就是气泡上下抖动。
void MessageHost::flushStreamingFrame()
{
	AgentMessageUnit* target = current() ? current()->lastAgentUnitIfLast() : nullptr;
	if (!target) {
		// 没有可渲染的气泡时别把刷新窗口挂着不放
		if (m_scrollArea)
			m_scrollArea->setUpdatesEnabled(true);
		return;
	}

	if (m_scrollArea)
		m_scrollArea->setUpdatesEnabled(false);

	target->flushStream();

	bool followBottom = false;
	if (m_scrollArea && m_scrollArea->verticalScrollBar()) {
		QScrollBar* bar = m_scrollArea->verticalScrollBar();
		// 不用"离底 80px 内"判定：死区比一格滚轮还大（一格 = 系统行数 3 × singleStep 20 = 60px，
		// 实测），上滚一格会被下一帧立刻拽回去。现在看"用户意图"（滚轮上滚即置 false；该信号同步
		// 发出，此刻位置还没变，所以不能只看位置）或"真的贴底"（2px 吸收取整误差）；内容长高导致
		// 位置离底时意图仍是"跟随"，流式输出不会因此停止跟随。
		const bool atBottom = bar->value() >= bar->maximum() - 2;
		followBottom = (m_followBottom || atBottom)
			&& !(m_wheelScroller && m_wheelScroller->isAnimating());
	}

	if (followBottom) {
		// 跟随底部：scrollToBottomNow() 自己会恢复刷新并滚到最新底部
		scrollToBottomNow();
	}
	else {
		// 不跟随（用户正在读旧内容）：只把布局落定 + 恢复刷新，绝不改滚动位置
		settleStreamingFrame();
	}
}

// 消息列（滚动区里的内容控件）必须"先长够高度，再铺布局"：直接 layout()->activate() 会在内容
// 控件仍是上一帧旧高度时给子部件分配几何，刚变高的气泡被就地挤扁（子部件从 2000+ px 压到几十
// px），表现为流式输出后半段被吞掉、滚动条范围缩水、拉不到底，下一轮再撑开，如此反复就成了闪烁。
void MessageHost::fitContentThenLayout()
{
	if (!m_scrollArea)
		return;

	QWidget* content = m_scrollArea->widget();
	if (!content)
		return;

	QLayout* contentLayout = content->layout();
	if (!contentLayout)
		return;

	contentLayout->invalidate();
	const int wanted = qMax(m_scrollArea->viewport()->height(), content->sizeHint().height());
	if (content->height() < wanted)
		content->resize(content->width(), wanted);

	contentLayout->activate();
}

// 流式渲染的“落定”收尾（不跟随底部时使用）：等本帧排队的二次拟合、布局都跑完再把这一帧一次性
// 画出来，并且**不动滚动位置**（用户正在读旧内容）。
void MessageHost::settleStreamingFrame()
{
	QTimer::singleShot(0, this, [this]() {
		if (!m_scrollArea)
			return;

		fitContentThenLayout();

		m_scrollArea->setUpdatesEnabled(true);
		m_scrollArea->viewport()->update();
		});
}

void MessageHost::onSendClicked()
{
	if (!m_chatInput)
		return;

	const QString text = m_chatInput->text().trimmed();
	if (text.isEmpty())
		return;

	if (m_sessionId.isEmpty()) {
		addSystemMessage(qtTrId("session_auto_creating"));
		// 建会话要动侧边栏与会话列表（DSHHub 的活），这里只把要发的内容交出去；
		// 建好后 DSHHub 会调 sendPrompt() 回来。
		emit sendWithoutSession(text);
		return;
	}

	sendPrompt(text);
}

void MessageHost::stopStreaming()
{
	m_streaming = false;
	if (m_streamTimer)
		m_streamTimer->stop();
	updateStreamingUi();
}

MessageHost::StreamOutcome MessageHost::onStreamEvent(const QJsonObject& event)
{
	if (!m_messages)
		return StreamOutcome::Ignored;

	// 事件到气泡内容的解析与更新下沉到 MessageQuery::applyStreamEvent，
	// 这里只保留控制器策略：streaming 标志 / 节流定时器 / 输入区按钮状态。
	const MessageQuery::StreamFrameResult streamResult =
		m_messages->applyStreamEvent(event, m_layout, m_streaming);

	if (streamResult.kind == MessageQuery::StreamFrameResult::FinalMessage) {
		// assistant/message 收尾：结束流式渲染状态（内容已在内部合并/封存）
		stopStreaming();
		return StreamOutcome::Finished;
	}
	if (streamResult.kind == MessageQuery::StreamFrameResult::Streaming) {
		// 收到流式内容（chunk/tool 等）：进入流式态，节流 50ms 批量重渲染
		m_streaming = true;
		if (m_streamTimer && !m_streamTimer->isActive())
			m_streamTimer->start();
		updateStreamingUi();
		return StreamOutcome::Streaming;
	}
	return StreamOutcome::Ignored;
}

// mux 帧路由（原 DSHHub::handleMuxFrame，随交互面板所有权一起搬来）：session/event 记 seq、过滤
// 本会话并渲染（窗口侧只剩标题刷新）；question/requested 与 approval/requested 建面板挂在列表里。
// 面板的"建"与"收"都在本类——建的时候收进 m_interactionPanels，收的时候走 clearInteractionPanels()，
// 两者必须待在一起，否则会漏回收。
void MessageHost::handleMuxFrame(const QJsonObject& frame)
{
	const QJsonObject payload = frame.value(QStringLiteral("payload")).toObject();
	const QString type = payload.value(QStringLiteral("type")).toString();
	const QString frameSessionId = payload.value(QStringLiteral("sessionId")).toString();

	if (type == QStringLiteral("session/event")) {
		const QJsonObject event = payload.value(QStringLiteral("event")).toObject();
		// 记录本会话见过的最新 seq：缓存快照用它判断缓存是否已被后来事件超越
		const int eventSeq = event.value(QStringLiteral("seq")).toInt();
		if (eventSeq > m_sessionLastSeq)
			m_sessionLastSeq = eventSeq;
		// 只渲染当前会话的事件：防止在 A 会话输出时切到 B 会话，A 的流式内容错误地显示在 B 里
		// （0.1.5 只跟随当前会话，所以"别的会话的帧"正常不会出现；真出现也直接丢弃）。
		if (!frameSessionId.isEmpty() && frameSessionId != m_sessionId)
			return;

		// "事件 → 气泡内容"的解析、streaming 标志 / 节流定时器 / 输入区按钮状态都在 onStreamEvent
		// 里；这里只管收尾之后的事。
		if (onStreamEvent(event) == StreamOutcome::Finished) {
			// 一轮对话完成后刷新会话标题（如果服务端已经生成了）。标题归侧栏，本类不碰。
			emit turnFinished();
			// 小灰字不用在这里补：本轮产生的投影变化由 session/control 流实时推过来
		}
	}
	else if (type == QStringLiteral("question/requested")) {
		QWidget* panel = InteractionHandler::handleQuestion(frame, m_api, m_layout);
		if (panel) {
			m_interactionPanels.append(panel);
			scrollToBottomNow();
			dshRegister(QStringLiteral("MessageHost.panel.%1").arg(reinterpret_cast<quintptr>(panel)),
				panel, qOverload<QObject*>(&QObject::destroyed), this, [this, panel]() {
					m_interactionPanels.removeAll(panel);
				});
		}
		else if (current()) {
			addSystemMessage(qtTrId("ask_panel_create_failed"));
		}
	}
	else if (type == QStringLiteral("approval/requested")) {
		QWidget* panel = InteractionHandler::handleApproval(frame, m_api, m_layout);
		if (panel) {
			m_interactionPanels.append(panel);
			scrollToBottomNow();
			dshRegister(QStringLiteral("MessageHost.panel.%1").arg(reinterpret_cast<quintptr>(panel)),
				panel, qOverload<QObject*>(&QObject::destroyed), this, [this, panel]() {
					m_interactionPanels.removeAll(panel);
				});
		}
		else if (current()) {
			addSystemMessage(qtTrId("ask_approval_panel_create_failed"));
		}
	}
}

void MessageHost::clearInteractionPanels()
{
	for (QWidget* panel : m_interactionPanels) {
		if (!panel)
			continue;

		if (m_layout)
			m_layout->removeWidget(panel);
		panel->hide();
		panel->deleteLater();
	}
	m_interactionPanels.clear();
}
