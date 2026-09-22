#include "ui/ChatInputWidget.h"

#include "ui/ModelSelector.h"
#include "ui/ShadowPanel.h"
#include "common/appearance/ThemeManager.h"

#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLayout>
#include <QPainter>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSize>
#include <QSizePolicy>
#include <QTextOption>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace
{
	// 卡片顶部留白（原生 composer 的 padding-top: 10px）
	constexpr int kCardTopPadding = 10;
	// 输入框与控制行之间的间距（原生 card gap: 12px）
	constexpr int kCardGap = 12;
	// 控制行内边距（原生 row padding: 2px 8px 6px）
	constexpr int kRowTopPadding = 2;
	constexpr int kRowSidePadding = 8;
	constexpr int kRowBottomPadding = 6;

	// 输入框在 QSS 里的上下 padding 之和（chat.qss: padding: 4px 12px 0 16px），
	// adjustHeight() 用它把文档高度换算成控件高度
	constexpr int kEditorVerticalPadding = 4;

	constexpr int kEditorMinHeight = 40;
	// 原生 --dsh-composer-text-max-height: 336px
	constexpr int kEditorMaxHeight = 336;

	// 卡片下方小灰字的字号（行高固定 14px，见 SessionStatsLine::kHeight）。
	// 字号在代码里也设一遍：外部定制的 styles 目录里没有新规则时，
	// 只靠 QSS 会回落到默认字号（比 14px 高），那行字就会被切掉。
	constexpr int kStatsFontPixelSize = 12;

	// ------------------------------------------------------------------
	// 小灰字的数字格式化
	// ------------------------------------------------------------------
	// 规则与官方 Web 端 chat/StatsLine 里的同名函数逐条对齐（单位、小数位、
	// 何时进位都一致），这样两端的同一份投影数据看起来是同一个东西。

	/** 时长：一分钟以内 "45.2s"，以上 "2m42s"。 */
	QString formatDuration(double ms)
	{
		const double seconds = ms / 1000.0;
		if (seconds < 60.0)
			return QStringLiteral("%1s").arg(std::round(seconds * 10.0) / 10.0, 0, 'g', 10);

		const qint64 whole = static_cast<qint64>(std::llround(seconds));
		return QStringLiteral("%1m%2s").arg(whole / 60).arg(whole % 60);
	}

	/** token 数：517 / 12.2K / 517K / 1.2M（三位以上保留一位小数，够三位就取整）。 */
	QString formatTokens(qint64 tokens)
	{
		if (tokens < 1000)
			return QString::number(tokens);

		const auto scaled = [](double value) {
			if (value >= 100.0)
				return QString::number(static_cast<qint64>(std::llround(value)));
			return QString::number(std::round(value * 10.0) / 10.0, 'g', 10);
			};

		if (tokens < 1000000)
			return scaled(tokens / 1000.0) + QStringLiteral("K");
		return scaled(tokens / 1000000.0) + QStringLiteral("M");
	}

	/** 解码吞吐：两位数以上取整，个位数留一位小数。 */
	QString formatThroughput(double tokensPerSecond)
	{
		const double clamped = tokensPerSecond > 0.0 ? tokensPerSecond : 0.0;
		if (clamped >= 10.0)
			return QString::number(static_cast<qint64>(std::llround(clamped)));
		return QString::number(std::round(clamped * 10.0) / 10.0, 'g', 10);
	}

	/**
	 * 缓存命中率：命中读 / 计费输入（未缓存 + 缓存读 + 缓存写）。
	 *
	 * 官方实现（StatsLine::cacheHitPercent）在"四舍五入会变成 100%、但其实没全中"
	 * 的时候多给几位小数（所以线上显示的是 99.6% 而不是 100%）。这里保留同样的
	 * 取舍：整数百分比 < 100 就取整，会进到 100 就逐步加小数位，直到严格小于 100。
	 * 没有计费输入时返回空串（这一组不显示）。
	 */
	QString formatCacheHitPercent(qint64 cacheReadTokens, qint64 billedInputTokens)
	{
		if (billedInputTokens <= 0)
			return QString();

		if (cacheReadTokens >= billedInputTokens)
			return QStringLiteral("100");

		const double percent = 100.0 * static_cast<double>(cacheReadTokens)
			/ static_cast<double>(billedInputTokens);

		if (qRound(percent) < 100)
			return QString::number(qRound(percent));

		for (int decimals = 1; decimals <= 4; ++decimals) {
			const double factor = std::pow(10.0, decimals);
			const double rounded = std::round(percent * factor) / factor;
			if (rounded < 100.0)
				return QString::number(rounded, 'f', decimals);
		}

		// 理论上到不了这里（percent 严格小于 100）；留个兜底免得画出 "100%"
		return QStringLiteral("99.99");
	}
}

// ------------------------------------------------------------------
// SessionStatsLine：卡片下方那行小灰字（本控件只负责画）
// ------------------------------------------------------------------

SessionStatsLine::SessionStatsLine(QWidget* parent)
	: QWidget(parent)
{
	setObjectName(QStringLiteral("sessionStatsLine"));
	// 固定行高 = 输入区留白里留给小灰字的那一条
	setFixedHeight(kHeight);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	// 字号在这里设一遍（理由见 kStatsFontPixelSize 的注释）
	QFont lineFont = font();
	lineFont.setPixelSize(kStatsFontPixelSize);
	setFont(lineFont);

	// 不设 WA_TransparentForMouseEvents：那会让 tooltip（文本被截断时的完整内容）
	// 也收不到悬停事件。这行本身没有点击语义，收到事件也不做任何事。

	// 初始就摆上"零状态"那一行（0 轮 · 0 步 | 输入 0 tok · 输出 0 tok）：
	// 新会话在服务端投影送到之前也该显示这一行，而不是先空着再冒出来。
	m_lineText = formatStats(SessionUsageStats{});
}

void SessionStatsLine::setStats(const SessionUsageStats& stats)
{
	setLineText(formatStats(stats));
}

void SessionStatsLine::setLineText(const QString& line)
{
	if (m_lineText == line)
		return;

	m_lineText = line;
	syncToolTip();
	update();
}

void SessionStatsLine::clearStats()
{
	// 注意：这里不是"擦成空白"，而是回到零状态。
	// 换会话时会调用本函数清掉上一会话的数字（见 DSHHub 的切换逻辑），
	// 而"始终显示"的约定要求新会话下面照样有这一行 —— 于是清成 0，
	// 等服务端投影到了再变成真实数字。
	setLineText(formatStats(SessionUsageStats{}));
}

void SessionStatsLine::paintEvent(QPaintEvent* event)
{
	Q_UNUSED(event)

		if (m_lineText.isEmpty())
			return;

	QPainter painter(this);
	painter.setFont(font());
	// 颜色取主题的"第三级文字"（和官方 StatsLine 的 label-tertiary 同一个角色）；
	// 主题切换会重建主窗口，所以这里不需要跟着变
	painter.setPen(CardShadow::parseColor(ThemeManager::instance().color(QStringLiteral("textTertiary"))));

	// 一行、居中，放不下就省略号（完整内容在 tooltip 里）
	const QFontMetrics metrics(font());
	const QString shown = metrics.elidedText(m_lineText, Qt::ElideRight, width());
	painter.drawText(rect(), Qt::AlignHCenter | Qt::AlignVCenter, shown);
}

void SessionStatsLine::resizeEvent(QResizeEvent* event)
{
	QWidget::resizeEvent(event);
	// 宽度变了，是否被截断（要不要挂 tooltip）可能跟着变
	syncToolTip();
}

void SessionStatsLine::syncToolTip()
{
	if (m_lineText.isEmpty()) {
		setToolTip(QString());
		return;
	}

	const QFontMetrics metrics(font());
	const bool truncated = metrics.horizontalAdvance(m_lineText) > width();
	setToolTip(truncated ? m_lineText : QString());
}

/**
 * 拼整行：组的顺序、分隔符、每组内部显示什么，都与官方 Web 端 StatsLine 一致 ——
 *   轮/步 | LLM 用时 · 工具调用用时 | 首 token 平均 · 吞吐 | 缓存命中 | 输入 · 输出
 * 组之间 " | "，组内 " · "。
 *
 * 与官方有一处**有意差异**：官方在没有任何可显示内容时直接 `return null`（整行不渲染），
 * 于是新会话下面一片空白。这里改成"始终显示"——「轮/步」与「输入/输出」两组无条件出现，
 * 所以新会话看到的是 `0 轮 · 0 步 | 输入 0 tok · 输出 0 tok`。
 * 其余组（LLM 用时 / 工具调用 / 首 token / tok/s / 缓存命中）没有有意义的 0 表示，
 * 维持"为 0 就不出现"，免得拼出一串假的 0s。
 */
QString SessionStatsLine::formatStats(const SessionUsageStats& stats)
{
	QStringList groups;

	// ---- 轮 / 步：始终显示 ----
	groups << qtTrId("chat_turn_step_fmt").arg(stats.turns).arg(stats.steps);

	// ---- 耗时：有值才出现 ----
	QStringList durations;
	if (stats.llmMs > 0)
		durations << qtTrId("chat_llm_label_fmt").arg(formatDuration(static_cast<double>(stats.llmMs)));
	if (stats.toolMs > 0)
		durations << qtTrId("chat_tool_call_count_fmt").arg(formatDuration(static_cast<double>(stats.toolMs)));
	if (!durations.isEmpty())
		groups << durations.join(QStringLiteral(" · "));

	// ---- 速率：有值才出现 ----
	QStringList speeds;
	if (stats.ttftSteps > 0) {
		// 首 token 是"平均"：累计延迟 / 记下首 token 的步数
		const double averageMs = static_cast<double>(stats.ttftMs) / stats.ttftSteps;
		speeds << qtTrId("chat_first_token_avg_fmt").arg(formatDuration(averageMs));
	}
	if (stats.decodeMs > 0) {
		const double tokensPerSecond = static_cast<double>(stats.decodeTokens)
			/ (static_cast<double>(stats.decodeMs) / 1000.0);
		speeds << qtTrId("chat_tok_per_sec_fmt").arg(formatThroughput(tokensPerSecond));
	}
	if (!speeds.isEmpty())
		groups << speeds.join(QStringLiteral(" · "));

	// ---- 输入 / 输出：始终显示 ----
	// 计费输入的三个桶是不重叠的：未命中 + 缓存读 + 缓存写
	const qint64 billedInput = stats.uncachedInputTokens
		+ stats.cacheReadTokens + stats.cacheWriteTokens;

	// 缓存命中率没有可算的分母（计费输入为 0）时整组不出现
	const QString cacheHit = formatCacheHitPercent(stats.cacheReadTokens, billedInput);
	if (!cacheHit.isEmpty())
		groups << qtTrId("chat_cache_hit_fmt").arg(cacheHit);

	groups << qtTrId("chat_token_usage_fmt")
		.arg(formatTokens(billedInput))
		.arg(formatTokens(stats.outputTokens));

	return groups.join(QStringLiteral(" | "));
}

// ------------------------------------------------------------------
// ChatInputWidget：卡片本体 + 卡片下方那行小灰字
// ------------------------------------------------------------------

CardShadow::Spec ChatInputWidget::shadowSpec()
{
	// 与原版输入卡片的 --dsw-shadow-lv2（0 4px 12px / 0 2px 8px，两层）同档：
	// 扩散 12 / 下移 3，圆角跟 #inputCapsule 的 22px 一致。
	//
	// 注意：卡片在外壳里"摆哪儿"不在这里调 —— 见构造里的 setPadding。
	// dy 只管阴影方向，挪位置用留白覆盖值，两件事分开。
	return CardShadow::Spec{ 12, 3, 22 };
}

ChatInputWidget::ChatInputWidget(QWidget* parent)
	: QWidget(parent)
{
	// 卡片本体（外观规则见 resources/styles/chat.qss 的 #inputCapsule /
	// #inputCapsule QPlainTextEdit / #inputControlRow）
	buildCapsule();

	// 卡片外面套阴影外壳（悬浮感）。外壳只画阴影，卡片的 QSS 规则不受影响。
	// 阴影色用 shadowSubtle（比 shadow 淡一档）：输入卡片是常驻在视线里的，
	// 给到和消息气泡一样重会显得"压"。
	auto* capsuleShadow = new ShadowPanel(QStringLiteral("shadowSubtle"), shadowSpec(), this);
	capsuleShadow->setRadius(shadowSpec().radius);
	capsuleShadow->setCard(m_capsule);

	// 卡片在外壳里的落位：在外壳"按 spec 推出来的自然位置"上整体下移。
	//
	// 只挪位置、外壳总高保持不变（上下留白一增一减、和不变），所以：
	//   · 卡片自己往下走 kCapsuleDrop 像素；
	//   · 外壳高度不变 → 下方小灰字的位置不变 → 它与侧栏卡片底边的齐平不受影响。
	// 阴影的方向与形状仍由 shadowSpec() 决定，不受这里影响。
	constexpr int kCapsuleDrop = 6;
	const QMargins pad = CardShadow::padding(shadowSpec());
	capsuleShadow->setPadding(QMargins(pad.left(), pad.top() + kCapsuleDrop,
		pad.right(), qMax(0, pad.bottom() - kCapsuleDrop)));

	// 卡片下方的小灰字统计（高度固定，没数据时画的是"零状态"那一行）
	m_statsLine = new SessionStatsLine(this);

	// 竖排：卡片在上、小灰字在下。两者之间不留间距 —— 上下留白由外层输入区
	// 的边距给（见 Main.cpp）
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(capsuleShadow);
	layout->addWidget(m_statsLine);

	m_editor->installEventFilter(this);

	connect(m_editor->document(), &QTextDocument::contentsChanged, this, [this]() {
		// 放到事件循环里再算，确保 QPlainTextEdit 已经用当前 viewport 宽度完成内部布局
		QTimer::singleShot(0, this, [this]() { adjustHeight(); });
		});

	QTimer::singleShot(0, this, [this]() { adjustHeight(); });

	retranslateUi();
}

void ChatInputWidget::buildCapsule()
{
	m_capsule = new QWidget(this);
	m_capsule->setObjectName(QStringLiteral("inputCapsule"));	// 让 QWidget 子类真正绘制样式表里的背景和边框
	m_capsule->setAttribute(Qt::WA_StyledBackground, true);

	m_editor = new QPlainTextEdit(m_capsule);
	m_editor->setFrameShape(QFrame::NoFrame);
	// 隐藏输入框滚动条，但保留鼠标滚轮滚动能力
	m_editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	// 必须显式开启 WidgetWidth 换行，再配合 wordWrapMode 控制断行方式。
	m_editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
	m_editor->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	m_editor->setFixedHeight(kEditorMinHeight);
	m_editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	buildControlRow();

	// 卡片：输入框在上，控制行在下（间距走布局 spacing，与原生 gap:12px 一致）
	auto* capsuleLayout = new QVBoxLayout(m_capsule);
	capsuleLayout->setContentsMargins(0, kCardTopPadding, 0, 0);
	capsuleLayout->setSpacing(kCardGap);
	capsuleLayout->addWidget(m_editor);
	capsuleLayout->addWidget(m_controlRow);
}

void ChatInputWidget::retranslateUi()
{
	if (m_editor)
		m_editor->setPlaceholderText(qtTrId("chat_input_placeholder"));

	// 发送键的提示跟输出状态绑定：交给同一个入口按当前状态重设
	if (m_sendButton)
		m_sendButton->setToolTip(m_streaming ? qtTrId("chat_stop_output") : qtTrId("common_send"));
}

void ChatInputWidget::changeEvent(QEvent* event)
{
	QWidget::changeEvent(event);

	if (event->type() == QEvent::LanguageChange)
		retranslateUi();
}

void ChatInputWidget::buildControlRow()
{
	m_controlRow = new QWidget(m_capsule);
	m_controlRow->setObjectName(QStringLiteral("inputControlRow"));

	auto* rowLayout = new QHBoxLayout(m_controlRow);
	rowLayout->setContentsMargins(kRowSidePadding, kRowTopPadding, kRowSidePadding, kRowBottomPadding);
	rowLayout->setSpacing(12);

	// 左侧控制组（原生 tools：gap 16px）——后续新增控制按键挂这里
	m_toolsLayout = new QHBoxLayout;
	m_toolsLayout->setContentsMargins(0, 0, 0, 0);
	m_toolsLayout->setSpacing(16);

	// 模型 / 思考档位：同一个控件（无会话 / 目录为空时自隐藏）
	m_modelSelector = new ModelSelector(m_controlRow);
	connect(m_modelSelector, &ModelSelector::modelChanged,
		this, &ChatInputWidget::modelChanged);
	connect(m_modelSelector, &ModelSelector::levelChanged,
		this, &ChatInputWidget::thinkingDepthChanged);
	m_toolsLayout->addWidget(m_modelSelector, 0, Qt::AlignVCenter);

	// 右侧控制组（原生 trailing：gap 12px，发送键也在其中）
	m_trailingLayout = new QHBoxLayout;
	m_trailingLayout->setContentsMargins(0, 0, 0, 0);
	m_trailingLayout->setSpacing(12);

	m_sendButton = new QPushButton(m_controlRow);
	m_sendButton->setObjectName(QStringLiteral("sendButton"));
	// 使用内置到 exe 的 EnterBtn.png 作为发送按钮图标
	m_sendButton->setIcon(QIcon(QStringLiteral(":/DSHHub/EnterBtn.png")));
	m_sendButton->setIconSize(QSize(32, 32));
	m_sendButton->setCursor(Qt::PointingHandCursor);
	m_sendButton->setToolTip(qtTrId("common_send"));
	m_sendButton->setFixedSize(32, 32);

	// 真正的“蒙版”是盖在图标上方的子控件；QSS background 会被图标遮住
	m_sendOverlay = new QWidget(m_sendButton);
	m_sendOverlay->setGeometry(0, 0, 32, 32);
	m_sendOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
	m_sendOverlay->setAttribute(Qt::WA_StyledBackground, true);
	m_sendOverlay->hide();
	m_sendButton->installEventFilter(this);

	m_trailingLayout->addWidget(m_sendButton, 0, Qt::AlignVCenter);

	rowLayout->addLayout(m_toolsLayout);
	rowLayout->addStretch(1);
	rowLayout->addLayout(m_trailingLayout);

	connect(m_sendButton, &QPushButton::clicked, this, &ChatInputWidget::handleSendClicked);
}

void ChatInputWidget::setSessionStats(const SessionUsageStats& stats)
{
	if (m_statsLine)
		m_statsLine->setStats(stats);
}

void ChatInputWidget::clearSessionStats()
{
	if (m_statsLine)
		m_statsLine->clearStats();
}

void ChatInputWidget::setModelSession(DshApiClient* api, const QString& sessionId)
{
	if (m_modelSelector)
		m_modelSelector->setSession(api, sessionId);
}

void ChatInputWidget::refreshModelCatalog()
{
	if (m_modelSelector)
		m_modelSelector->refresh();
}

void ChatInputWidget::applySessionModelSelection(const QString& provider, const QString& model,
	const QString& reasoningEffort)
{
	if (m_modelSelector)
		m_modelSelector->overrideCurrentSelection(provider, model, reasoningEffort);
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

void ChatInputWidget::setStreaming(bool streaming)
{
	if (m_streaming == streaming)
		return;

	m_streaming = streaming;

	if (!m_sendButton)
		return;

	if (m_streaming) {
		m_sendButton->setIcon(QIcon(QStringLiteral(":/DSHHub/StopBtn.png")));
		m_sendButton->setToolTip(qtTrId("chat_stop_output"));
	}
	else {
		m_sendButton->setIcon(QIcon(QStringLiteral(":/DSHHub/EnterBtn.png")));
		m_sendButton->setToolTip(qtTrId("common_send"));
	}
}

void ChatInputWidget::handleSendClicked()
{
	if (m_streaming) {
		emit stopRequested();
		return;
	}

	if (m_editor)
		emit sendRequested(m_editor->toPlainText());
}

bool ChatInputWidget::eventFilter(QObject* obj, QEvent* event)
{
	if (obj == m_editor && event->type() == QEvent::KeyPress) {
		auto* keyEvent = static_cast<QKeyEvent*>(event);
		if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)
			&& !(keyEvent->modifiers() & Qt::ShiftModifier)) {
			if (m_streaming)
				emit stopRequested();
			else
				emit sendRequested(m_editor->toPlainText());
			return true;
		}
	}
	else if (obj == m_editor && event->type() == QEvent::Resize) {
		// 输入框宽度变化后，换行位置/高度需要重新计算
		QTimer::singleShot(0, this, [this]() { adjustHeight(); });
	}
	else if (obj == m_sendButton) {
		switch (event->type()) {
		case QEvent::Enter:
			m_sendHovered = true;
			updateSendOverlay();
			break;
		case QEvent::Leave:
			m_sendHovered = false;
			updateSendOverlay();
			break;
		case QEvent::MouseButtonPress:
			m_sendPressed = true;
			updateSendOverlay();
			break;
		case QEvent::MouseButtonRelease:
			m_sendPressed = false;
			updateSendOverlay();
			break;
		default:
			break;
		}
	}

	return QWidget::eventFilter(obj, event);
}

void ChatInputWidget::updateSendOverlay()
{
	if (!m_sendOverlay)
		return;

	if (m_sendPressed) {
		m_sendOverlay->setStyleSheet(
			QStringLiteral("background: rgba(255, 255, 255, 0.55); border-radius: 16px;"));
		m_sendOverlay->show();
	}
	else if (m_sendHovered) {
		m_sendOverlay->setStyleSheet(
			QStringLiteral("background: rgba(255, 255, 255, 0.35); border-radius: 16px;"));
		m_sendOverlay->show();
	}
	else {
		m_sendOverlay->hide();
	}
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
	// 再加上 QSS 中 QPlainTextEdit 的上下 padding
	const int contentHeight = static_cast<int>(docHeight * lineHeight + 0.5) + kEditorVerticalPadding;
	const int newHeight = qBound(kEditorMinHeight, contentHeight, kEditorMaxHeight);

	if (m_editor->height() != newHeight) {
		m_editor->setFixedHeight(newHeight);
		m_editor->updateGeometry();

		// 通知父级布局重新计算：输入框高度是 setFixedHeight 直接定的，卡片本体的
		// sizeHint 跟着变，再往上还有"本控件（卡片 + 小灰字）"和输入区两层容器，
		// 不逐层重算一次，外层会一直按旧高度摆到下一次窗口事件。
		QWidget* level = m_editor->parentWidget();
		for (int depth = 0; level && depth < 3; ++depth) {
			level->updateGeometry();
			if (QLayout* levelLayout = level->layout())
				levelLayout->activate();
			level = level->parentWidget();
		}
	}

	// 如果内容完整可见，要把滚动位置拉回顶部。
	// 否则 QPlainTextEdit 之前为了跟随光标产生的滚动偏移会让第一行被顶出视野。
	if (contentHeight <= newHeight)
		m_editor->verticalScrollBar()->setValue(0);
}