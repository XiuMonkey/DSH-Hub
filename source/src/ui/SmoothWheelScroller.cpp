#include "ui/SmoothWheelScroller.h"
#include "core/ConnectionManager.h"

#include <QAbstractAnimation>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QEasingCurve>
#include <QPropertyAnimation>
#include <QScrollBar>
#include <QWheelEvent>
#include <QWidget>
#include <cmath>

namespace
{
	// 补间时长随距离增长但夹在区间内
	constexpr int kMinDurationMs = 80;
	constexpr int kMaxDurationMs = 220;
	constexpr double kMsPerPixel = 2.2;

	// 滚轮步长跟着系统"每次滚动下列行数"走
	int wheelLines()
	{
		const int lines = QApplication::wheelScrollLines();
		return lines > 0 ? lines : 1;
	}
} // namespace

SmoothWheelScroller::SmoothWheelScroller(QAbstractScrollArea* area, QObject* parent)
	: QObject(parent ? parent : static_cast<QObject*>(area))
	, m_area(area)
{
	if (!m_area)
		return;

	// 装在视口上才能拦在 Qt 默认处理之前
	m_area->viewport()->installEventFilter(this);

	m_animation = new QPropertyAnimation(this);
	m_animation->setTargetObject(m_area->verticalScrollBar());
	m_animation->setPropertyName("value");
	m_animation->setEasingCurve(QEasingCurve::OutCubic);
	const QString scrollerIndex = QStringLiteral("SmoothWheelScroller.%1").arg(reinterpret_cast<quintptr>(this));
	dshRegister(scrollerIndex + QStringLiteral(".finish"), m_animation, &QPropertyAnimation::finished, this, [this]() {
		syncTargetToBar();

		// 追新底部，否则补间这 100 多毫秒里内容长高后跟随会断裂
		QScrollBar* bar = m_area ? m_area->verticalScrollBar() : nullptr;
		if (m_aimedAtBottom && m_lastDeltaDown && bar && bar->maximum() > bar->value()) {
			m_target = static_cast<double>(bar->maximum());
			animateToTarget();
		}
	});

	if (QScrollBar* bar = m_area->verticalScrollBar()) {
		// 明确的用户操作，补间立刻让位（程序 setValue 不发这两个信号）
		dshRegister(scrollerIndex + QStringLiteral(".sliderPressed"), bar, &QScrollBar::sliderPressed, this,
			&SmoothWheelScroller::stop);
		dshRegister(scrollerIndex + QStringLiteral(".actionTriggered"), bar, &QScrollBar::actionTriggered, this,
			[this](int) { stop(); });
	}
	syncTargetToBar();
}

SmoothWheelScroller::~SmoothWheelScroller()
{
	// 滚动条销毁时机不归我们管，析构前先停下
	if (m_animation)
		m_animation->stop();
}

bool SmoothWheelScroller::isAnimating() const
{
	return m_animation && m_animation->state() == QAbstractAnimation::Running;
}

void SmoothWheelScroller::stop()
{
	if (m_animation && m_animation->state() != QAbstractAnimation::Stopped)
		m_animation->stop();

	syncTargetToBar();
}

void SmoothWheelScroller::syncTargetToBar()
{
	const QScrollBar* bar = m_area ? m_area->verticalScrollBar() : nullptr;
	m_target = bar ? static_cast<double>(bar->value()) : 0.0;
}

bool SmoothWheelScroller::eventFilter(QObject* watched, QEvent* event)
{
	if (m_area && event->type() == QEvent::Wheel && watched == m_area->viewport()) {
		if (handleWheel(static_cast<QWheelEvent*>(event)))
			return true;
	}

	return QObject::eventFilter(watched, event);
}

bool SmoothWheelScroller::handleWheel(QWheelEvent* event)
{
	if (!m_area)
		return false;

	QScrollBar* bar = m_area->verticalScrollBar();
	if (!bar || bar->maximum() <= bar->minimum())
		return false; // 这个视图没得滚：继续往上层滚动区传

	const QPoint pixelDelta = event->pixelDelta();
	const QPoint angleDelta = event->angleDelta();
	double delta = 0.0;
	if (!pixelDelta.isNull()) {
		// Qt 滚动区只认 angleDelta，只有我们这里才真正生效
		delta = -pixelDelta.y();
	}
	else if (angleDelta.y() != 0) {
		// 一格 = 120，换算成"系统行数 × singleStep"像素
		delta = -angleDelta.y() * (wheelLines() * bar->singleStep()) / 120.0;
	}

	// 小增量要累积，不能被取整吃掉
	if (std::abs(delta) < 0.01)
		return false;

	// 已贴边还往同方向滚：留给上层滚动区
	if (delta < 0 && bar->value() <= bar->minimum())
		return false;
	if (delta > 0 && !isAnimating() && bar->value() >= bar->maximum())
		return false;

	// 飞行中在当前目标上继续累积（叠加，不打断上一段补间）
	const double base = isAnimating() ? m_target : static_cast<double>(bar->value());
	m_target = qBound(static_cast<double>(bar->minimum()), base + delta, static_cast<double>(bar->maximum()));

	m_lastDeltaDown = delta > 0;
	if (delta < 0)
		emit userScrolledAway(); // 同步发出：此刻滚动条还贴着底部

	event->accept();
	animateToTarget();
	return true;
}

void SmoothWheelScroller::animateToTarget()
{
	QScrollBar* bar = m_area ? m_area->verticalScrollBar() : nullptr;
	if (!bar)
		return;

	const int start = bar->value();
	const int end = qBound(bar->minimum(), qRound(m_target), bar->maximum());
	if (end == start) {
		m_target = start;
		return;
	}

	// 从当前值起步：飞行中重定向是接着滑
	m_animation->stop();
	m_animation->setStartValue(start);
	m_animation->setEndValue(end);
	m_aimedAtBottom = (end >= bar->maximum());
	m_animation->setDuration(qBound(kMinDurationMs, static_cast<int>(std::abs(end - start) * kMsPerPixel),
		kMaxDurationMs));
	m_animation->start();
}
