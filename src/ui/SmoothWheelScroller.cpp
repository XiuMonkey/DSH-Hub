#include "SmoothWheelScroller.h"

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
	// 补间时长随距离增长，但夹在一个区间里：一格（约 60px）≈ 130ms，观感是"推过去"。
	// 短距离别慢吞吞（否则连续滚动会觉得黏），长距离（一屏）也别拖沓。
	constexpr int kMinDurationMs = 80;
	constexpr int kMaxDurationMs = 220;
	constexpr double kMsPerPixel = 2.2;

	// 系统"每次滚动下列行数"（Windows 设置里可改），Qt 默认 3。
	// 滚轮步长必须跟着它走：用户把系统行数改成 1 或 10，这里也就跟着变。
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

	// 滚轮事件落在视口上（QAbstractScrollArea 把视口事件转给自身的 wheelEvent），
	// 所以过滤器装在视口上就一定能拦在 Qt 默认处理之前。
	m_area->viewport()->installEventFilter(this);

	m_animation = new QPropertyAnimation(this);
	m_animation->setTargetObject(m_area->verticalScrollBar());
	m_animation->setPropertyName("value"); // QAbstractSlider::value
	m_animation->setEasingCurve(QEasingCurve::OutCubic);
	connect(m_animation, &QPropertyAnimation::finished, this, [this]() {
		syncTargetToBar();

		// 用户是往底部滚的，但补间这 100 多毫秒里内容又长高了（流式输出就是这样）：
		// 底部已经移走，接着追新的底部。否则会"停在离底几十像素的地方"，
		// 而宿主的跟随判定又要求真的贴底，于是跟随就这么断了。
		QScrollBar* bar = m_area ? m_area->verticalScrollBar() : nullptr;
		if (m_aimedAtBottom && m_lastDeltaDown && bar && bar->maximum() > bar->value()) {
			m_target = static_cast<double>(bar->maximum());
			animateToTarget();
		}
		});

	if (QScrollBar* bar = m_area->verticalScrollBar()) {
		// 用户直接拖把手 / 点箭头 / PageDown：都是明确的用户操作，补间立刻让位。
		// （程序 setValue 不会发这两个信号，所以补间自己不会被自己打断。）
		connect(bar, &QScrollBar::sliderPressed, this, &SmoothWheelScroller::stop);
		connect(bar, &QScrollBar::actionTriggered, this, [this](int) { stop(); });
	}

	syncTargetToBar();
}

SmoothWheelScroller::~SmoothWheelScroller()
{
	// 补间正在改滚动条的值，而滚动条的销毁时机不归我们管：析构前先停下
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
			return true; // 已接管：别再让 Qt 按 singleStep 一次性跳
	}

	return QObject::eventFilter(watched, event);
}

bool SmoothWheelScroller::handleWheel(QWheelEvent* event)
{
	if (!m_area)
		return false;

	QScrollBar* bar = m_area->verticalScrollBar();
	if (!bar || bar->maximum() <= bar->minimum())
		return false; // 这个视图自己没得滚：不吃事件，让它继续往上层滚动区传

	const QPoint pixelDelta = event->pixelDelta();
	const QPoint angleDelta = event->angleDelta();

	// 统一换算成"滚动位置的增量"（正数 = 往下滚）
	double delta = 0.0;
	if (!pixelDelta.isNull()) {
		// 高精度设备（触控板 / 带平滑驱动的鼠标）直接给像素增量。
		// 注意：Qt 默认的滚动区处理只认 angleDelta（实测只给 pixelDelta 时滚不动），
		// 所以这条分支是在我们这里才真正生效的。
		delta = -pixelDelta.y();
	}
	else if (angleDelta.y() != 0) {
		// 经典滚轮：一格 = 120，换算成"系统行数 × singleStep"像素。
		// 不写死 60：用户改了系统行数、或滚动区字号变了，步长都跟着变。
		delta = -angleDelta.y() * (wheelLines() * bar->singleStep()) / 120.0;
	}

	// 保留小数：高精度设备的 1/8 格（15px）之类的小增量要累积起来，不能被取整吃掉
	if (std::abs(delta) < 0.01)
		return false;

	// 已经贴边还要往同方向滚：不吃事件，留给上层滚动区（嵌套滚动链保持不变）
	if (delta < 0 && bar->value() <= bar->minimum())
		return false;
	if (delta > 0 && !isAnimating() && bar->value() >= bar->maximum())
		return false;

	// 飞行中在"当前目标"上继续累积：连续滚动是叠加，而不是把上一段补间打断重来
	const double base = isAnimating() ? m_target : static_cast<double>(bar->value());
	m_target = qBound(static_cast<double>(bar->minimum()), base + delta,
		static_cast<double>(bar->maximum()));

	m_lastDeltaDown = delta > 0;
	if (delta < 0)
		emit userScrolledAway(); // 同步发出：此刻滚动条还贴着底部，宿主只能靠它知道用户要走

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
		// 目标就是当前位置（例如只剩不到 1px 的余量）：没有补间可做
		m_target = start;
		return;
	}

	// 从"当前值"起步：飞行中重定向是接着滑，不是跳回去重来
	m_animation->stop();
	m_animation->setStartValue(start);
	m_animation->setEndValue(end);
	m_aimedAtBottom = (end >= bar->maximum());
	m_animation->setDuration(qBound(kMinDurationMs,
		static_cast<int>(std::abs(end - start) * kMsPerPixel), kMaxDurationMs));
	m_animation->start();
}