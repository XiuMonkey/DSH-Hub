#include "ui/StatusPopupWindow.h"

#include <QEvent>
#include <QFontMetrics>
#include <QLabel>

namespace
{
	// 把单行文本按可用宽度裁成最多两行：
	//   - 放得下就原样返回；
	//   - 两行放得下则从中间断行；
	//   - 第二行仍放不下时截断并补 …。
	QString statusTextTwoLines(const QString& text, int width, const QFontMetrics& fm)
	{
		if (text.isEmpty() || fm.horizontalAdvance(text) <= width)
			return text;

		const int ellipsisWidth = fm.horizontalAdvance(QStringLiteral("…"));

		// 从 start 开始按可用宽度贪心截取一行（新行符按宽度 0 处理，简单起见不换行语义）
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

		// 第二行也放不下：第二行截到“省略号也放得下”为止，末尾补 …
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

	// 布局后按真实宽度排版；还没拿到真实宽度时先全量显示，
	// 等 Resize 事件（布局真正生效）到来再按实际宽度决定是否省略。
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
	// 布局真正生效（标签宽度变化）时重算一次，避免早期按窄宽度错误截断
	if (watched == m_statusLabel && event->type() == QEvent::Resize)
		refreshStatusDisplay();
	return PopupWindow::eventFilter(watched, event);
}