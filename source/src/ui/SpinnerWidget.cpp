#include "ui/SpinnerWidget.h"
#include "core/ConnectionManager.h"
#include "common/appearance/ThemeManager.h"

#include <QPainter>
#include <QTimer>

SpinnerWidget::SpinnerWidget(QWidget* parent)
	: QWidget(parent)
{
	m_timer = new QTimer(this);
	m_timer->setInterval(30);

	// 转圈控件有多实例，index 带上自身地址
	dshRegister(QStringLiteral("SpinnerWidget.%1").arg(reinterpret_cast<quintptr>(this)),
		m_timer, &QTimer::timeout, this, [this]() {
			m_angle = (m_angle + 12) % 360;
			update();
		});
}

void SpinnerWidget::start()
{
	if (!m_timer->isActive())
		m_timer->start();
}

void SpinnerWidget::paintEvent(QPaintEvent* event)
{
	Q_UNUSED(event)

	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);

	const int side = qMin(width(), height());
	const QRectF rect(2, 2, side - 4, side - 4);

	// 背景圆环
	QPen bgPen(QColor(200, 200, 200, 120), 4, Qt::SolidLine, Qt::RoundCap);
	painter.setPen(bgPen);
	painter.drawArc(rect, 0, 360 * 16);

	// 前景旋转弧
	QPen fgPen(QColor(ThemeManager::instance().accent()), 4, Qt::SolidLine, Qt::RoundCap);
	painter.setPen(fgPen);
	painter.drawArc(rect, m_angle * 16, 270 * 16);
}
