#pragma once

// PopupWindow 的 UI 扩展：带一条“按真实宽度两行省略”的状态栏（插件市场与扩展管理弹窗共用，
// 排版逻辑是纯绘制辅助 QFontMetrics，因此留在 ui 层）。

#include "ui/PopupWindow.h"

#include <QString>

class QLabel;
class QEvent;

class StatusPopupWindow : public PopupWindow
{
	Q_OBJECT

public:
	explicit StatusPopupWindow(QWidget* parent = nullptr);

	// 设置状态栏完整文本（同时作为 tooltip），并按当前标签宽度重排显示
	void setStatus(const QString& text);

	// 把状态标签纳入宽度监听：布局变化（Resize）后自动重新排版
	void attachStatusLabel(QLabel* label);

	// 按“当前真实宽度”重排版状态文本（首次/宽度变化后）
	void refreshStatusDisplay();

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	QLabel* m_statusLabel = nullptr;
	QString m_lastStatusText;
};
