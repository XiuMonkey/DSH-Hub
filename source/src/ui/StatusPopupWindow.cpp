#include "ui/StatusPopupWindow.h"

#include <QEvent>
#include <QFontMetrics>
#include <QLabel>

namespace
{
	// 最多两行：放不下原样，否则中间断行补 …
	QString statusTextTwoLines(const QString& text, int width, const QFontMetrics& fm)
	{
		if (text.isEmpty() || fm.horizontalAdvance(text) <= width)
			return text;

		const int ellipsisWidth = fm.horizontalAdvance(QStringLiteral("…"));

		const auto takeLine = [&fm](const QString& src, int start, int maxWidth) -> QString {
			QString out;
			int used = 0;
			for (int i = start; i < src.size(); ++i) {
				const int charWidth = fm.horizontalAdvance(src.at(i));
				if (charWidth > 0 && used + charWidth > maxWidth)
					break;
				out += src.at(i);
				if (charWidth > 0)
					used += charWidth;
			}
			return out;
			};

		const QString first = takeLine(text, 0, width);
		const QString rest = text.mid(first.size());
		if (fm.horizontalAdvance(rest) <= width)
			return first + QLatin1Char('\n') + rest;

		const int secondMax = qMax(20, width - ellipsisWidth);
		QString second = takeLine(rest, 0, secondMax);
		if (rest.size() > second.size())
			second += QStringLiteral("…");
		return first + QLatin1Char('\n') + second;
	}
}

StatusPopupWindow::StatusPopupWindow(QWidget* parent)
	: PopupWindow(parent)
{
}

void StatusPopupWindow::attachStatusLabel(QLabel* label)
{
	m_statusLabel = label;
	if (!m_statusLabel)
		return;

	m_statusLabel->installEventFilter(this);
	refreshStatusDisplay();
}

void StatusPopupWindow::setStatus(const QString& text)
{
	if (!m_statusLabel)
		return;

	m_statusLabel->setToolTip(text);
	m_lastStatusText = text;
	refreshStatusDisplay();
}

void StatusPopupWindow::refreshStatusDisplay()
{
	if (!m_statusLabel)
		return;

	// 未拿到真实宽度时先全量显示，等 Resize 事件到达再按宽度省略
	int width = m_statusLabel->width();
	if (width <= 10) {
		const QWidget* win = m_statusLabel->window();
		width = win && win->width() > 40 ? win->width() - 40 : 0;
	}
	if (width <= 10) {
		m_statusLabel->setWordWrap(false);
		m_statusLabel->setText(m_lastStatusText);
		return;
	}

	m_statusLabel->setWordWrap(false);
	m_statusLabel->setText(statusTextTwoLines(m_lastStatusText, width, m_statusLabel->fontMetrics()));
}

bool StatusPopupWindow::eventFilter(QObject* watched, QEvent* event)
{
	// 宽度变化时重算，避免早期窄宽度截断
	if (watched == m_statusLabel && event->type() == QEvent::Resize)
		refreshStatusDisplay();
	return PopupWindow::eventFilter(watched, event);
}
