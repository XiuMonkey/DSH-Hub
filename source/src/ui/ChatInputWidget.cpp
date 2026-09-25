#include "ui/ChatInputWidget.h"
#include "core/ConnectionManager.h"
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
	// 尺寸常量对齐原生 composer
	constexpr int kCardTopPadding = 10;
	constexpr int kCardGap = 12;
	constexpr int kRowTopPadding = 2;
	constexpr int kRowSidePadding = 8;
	constexpr int kRowBottomPadding = 6;

	constexpr int kEditorVerticalPadding = 4;
	constexpr int kEditorMinHeight = 40;
	constexpr int kEditorMaxHeight = 336;

	// 字号也要在代码里设：外部 styles 缺新规则时只靠 QSS 会用更大字号，那行字被切掉
	constexpr int kStatsFontPixelSize = 12;

	QString formatDuration(double ms)
	{
		const double seconds = ms / 1000.0;
		if (seconds < 60.0)
			return QStringLiteral("%1s").arg(std::round(seconds * 10.0) / 10.0, 0, 'g', 10);

		const qint64 whole = static_cast<qint64>(std::llround(seconds));
		return QStringLiteral("%1m%2s").arg(whole / 60).arg(whole % 60);
	}

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

	// 吞吐：两位数以上取整，个位数留一位小数
	QString formatThroughput(double tokensPerSecond)
	{
		const double clamped = tokensPerSecond > 0.0 ? tokensPerSecond : 0.0;
		if (clamped >= 10.0)
			return QString::number(static_cast<qint64>(std::llround(clamped)));
		return QString::number(std::round(clamped * 10.0) / 10.0, 'g', 10);
	}

	// 命中率 = 命中读 / 计费输入；无计费输入时返回空串
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

		// 兜底，免得画出 "100%"
		return QStringLiteral("99.99");
	}
}

SessionStatsLine::SessionStatsLine(QWidget* parent)
	: QWidget(parent)
{
	setObjectName(QStringLiteral("sessionStatsLine"));
	setFixedHeight(kHeight);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	QFont lineFont = font();
	lineFont.setPixelSize(kStatsFontPixelSize);
	setFont(lineFont);

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
	// 回到零状态而非擦成空白：换会话要清掉旧数字，这一行必须始终显示
	setLineText(formatStats(SessionUsageStats{}));
}

void SessionStatsLine::paintEvent(QPaintEvent* event)
{
	Q_UNUSED(event)
	if (m_lineText.isEmpty())
		return;

	QPainter painter(this);
	painter.setFont(font());
	// 取主题的第三级文字；主题切换会重建主窗口，这里不用跟着变
	painter.setPen(CardShadow::parseColor(ThemeManager::instance().color(QStringLiteral("textTertiary"))));

	const QFontMetrics metrics(font());
	const QString shown = metrics.elidedText(m_lineText, Qt::ElideRight, width());
	painter.drawText(rect(), Qt::AlignHCenter | Qt::AlignVCenter, shown);
}

void SessionStatsLine::resizeEvent(QResizeEvent* event)
{
	QWidget::resizeEvent(event);
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

// 组序与分隔符同官方 StatsLine。有意差异：官方无可显示内容时不渲染，这里"轮/步"与"输入/输出"
// 无条件出现；其余组为 0 就不出现
QString SessionStatsLine::formatStats(const SessionUsageStats& stats)
{
	QStringList groups;

	groups << qtTrId("chat_turn_step_fmt").arg(stats.turns).arg(stats.steps);

	QStringList durations;
	if (stats.llmMs > 0)
		durations << qtTrId("chat_llm_label_fmt").arg(formatDuration(static_cast<double>(stats.llmMs)));
	if (stats.toolMs > 0)
		durations << qtTrId("chat_tool_call_count_fmt").arg(formatDuration(static_cast<double>(stats.toolMs)));
	if (!durations.isEmpty())
		groups << durations.join(QStringLiteral(" · "));

	QStringList speeds;
	if (stats.ttftSteps > 0) {
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

	// 计费输入的三个桶不重叠：未命中 + 缓存读 + 缓存写
	const qint64 billedInput = stats.uncachedInputTokens
		+ stats.cacheReadTokens + stats.cacheWriteTokens;

	const QString cacheHit = formatCacheHitPercent(stats.cacheReadTokens, billedInput);
	if (!cacheHit.isEmpty())
		groups << qtTrId("chat_cache_hit_fmt").arg(cacheHit);

	groups << qtTrId("chat_token_usage_fmt")
		.arg(formatTokens(billedInput))
		.arg(formatTokens(stats.outputTokens));

	return groups.join(QStringLiteral(" | "));
}

CardShadow::Spec ChatInputWidget::shadowSpec()
{
	// 同原版 --dsw-shadow-lv2 档：扩散 12 / 下移 3，圆角跟 #inputCapsule 的 22px 一致；
	// 卡片在外壳里的位置不在这里调（见构造里的 setPadding）
	return CardShadow::Spec{ 12, 3, 22 };
}

ChatInputWidget::ChatInputWidget(QWidget* parent)
	: QWidget(parent)
{
	buildCapsule();

	// 外壳只画阴影，不影响卡片 QSS；用 shadowSubtle（比 shadow 淡一档）
	auto* capsuleShadow = new ShadowPanel(QStringLiteral("shadowSubtle"), shadowSpec(), this);
	capsuleShadow->setRadius(shadowSpec().radius);
	capsuleShadow->setCard(m_capsule);

	// 在自然位置上整体下移 kCapsuleDrop：只挪位置、外壳总高不变，下方小灰字与侧栏底边的齐平不受影响
	constexpr int kCapsuleDrop = 6;
	const QMargins pad = CardShadow::padding(shadowSpec());
	capsuleShadow->setPadding(QMargins(pad.left(), pad.top() + kCapsuleDrop, pad.right(),
		qMax(0, pad.bottom() - kCapsuleDrop)));

	m_statsLine = new SessionStatsLine(this);

	// 卡片在上、小灰字在下，间距为 0；留白由外层输入区边距给（见 Main.cpp）
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(capsuleShadow);
	layout->addWidget(m_statsLine);

	m_editor->installEventFilter(this);

	dshRegister("ChatInputWidget.001", m_editor->document(), &QTextDocument::contentsChanged, this, [this]() {
			// 放到事件循环里再算，确保编辑器已用当前 viewport 宽度完成布局
			QTimer::singleShot(0, this, [this]() { adjustHeight(); });
		});

	QTimer::singleShot(0, this, [this]() { adjustHeight(); });

	retranslateUi();
}

void ChatInputWidget::buildCapsule()
{
	m_capsule = new QWidget(this);
	m_capsule->setObjectName(QStringLiteral("inputCapsule"));	// 让 QWidget 绘制样式表背景与边框
	m_capsule->setAttribute(Qt::WA_StyledBackground, true);

	m_editor = new QPlainTextEdit(m_capsule);
	m_editor->setFrameShape(QFrame::NoFrame);
	m_editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	// 必须显式开 WidgetWidth 换行，再由 wordWrapMode 控制断行
	m_editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
	m_editor->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	m_editor->setFixedHeight(kEditorMinHeight);
	m_editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	buildControlRow();

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

	// 左侧控制组（原生 gap 16px），新增控制按键挂这里
	m_toolsLayout = new QHBoxLayout;
	m_toolsLayout->setContentsMargins(0, 0, 0, 0);
	m_toolsLayout->setSpacing(16);

	// 模型 / 思考档位；无会话或目录为空时自隐藏
	m_modelSelector = new ModelSelector(m_controlRow);
	dshRegister("ChatInputWidget.002", m_modelSelector, &ModelSelector::modelChanged,
		this, &ChatInputWidget::modelChanged);
	dshRegister("ChatInputWidget.003", m_modelSelector, &ModelSelector::levelChanged,
		this, &ChatInputWidget::thinkingDepthChanged);
	m_toolsLayout->addWidget(m_modelSelector, 0, Qt::AlignVCenter);

	// 右侧控制组（原生 trailing gap 12px，含发送键）
	m_trailingLayout = new QHBoxLayout;
	m_trailingLayout->setContentsMargins(0, 0, 0, 0);
	m_trailingLayout->setSpacing(12);

	m_sendButton = new QPushButton(m_controlRow);
	m_sendButton->setObjectName(QStringLiteral("sendButton"));
	m_sendButton->setIcon(QIcon(QStringLiteral(":/DSHHub/EnterBtn.png")));
	m_sendButton->setIconSize(QSize(32, 32));
	m_sendButton->setCursor(Qt::PointingHandCursor);
	m_sendButton->setToolTip(qtTrId("common_send"));
	m_sendButton->setFixedSize(32, 32);

	// 蒙版得是盖在图标上方的子控件，QSS background 会被图标遮住
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

	dshRegister("ChatInputWidget.004", m_sendButton, qOverload<bool>(&QPushButton::clicked),
		this, &ChatInputWidget::handleSendClicked);
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
		m_sendOverlay->setStyleSheet(QStringLiteral("background: rgba(255, 255, 255, 0.55); border-radius: 16px;"));
		m_sendOverlay->show();
	}
	else if (m_sendHovered) {
		m_sendOverlay->setStyleSheet(QStringLiteral("background: rgba(255, 255, 255, 0.35); border-radius: 16px;"));
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

	if (m_editor->lineWrapMode() != QPlainTextEdit::WidgetWidth)
		m_editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);

	int textWidth = m_editor->viewport()->width();
	if (textWidth <= 0)
		textWidth = m_editor->width() - 4;
	if (textWidth <= 0)
		textWidth = 1;

	m_editor->document()->setTextWidth(textWidth);

	// 内部 QPlainTextDocumentLayout 才知道私有行宽；临时切一次 lineWrapMode 强制按当前 viewport 重排
	m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
	m_editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);

	// documentSize().height() 单位是行数而非像素，乘行距才是像素
	const qreal docHeight = m_editor->document()->documentLayout()->documentSize().height();
	const qreal lineHeight = m_editor->fontMetrics().lineSpacing();
	const int contentHeight = static_cast<int>(docHeight * lineHeight + 0.5) + kEditorVerticalPadding;
	const int newHeight = qBound(kEditorMinHeight, contentHeight, kEditorMaxHeight);

	if (m_editor->height() != newHeight) {
		m_editor->setFixedHeight(newHeight);
		m_editor->updateGeometry();

		// 逐层通知父布局重算，否则外层会一直按旧高度摆
		QWidget* level = m_editor->parentWidget();
		for (int depth = 0; level && depth < 3; ++depth) {
			level->updateGeometry();
			if (QLayout* levelLayout = level->layout())
				levelLayout->activate();
			level = level->parentWidget();
		}
	}

	// 内容完整可见时把滚动拉回顶部，否则第一行会被顶出视野
	if (contentHeight <= newHeight)
		m_editor->verticalScrollBar()->setValue(0);
}
