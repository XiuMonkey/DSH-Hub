#pragma once

// 清空布局这类"装配样板"的共用实现。

#include <QLayout>
#include <QLayoutItem>
#include <QWidget>

namespace LayoutUtils
{
	// 清空时旧控件怎么处理。三档的差异都是有意保留的，别合并。
	enum class ClearMode
	{
		// hide + 摘离控件树 + deleteLater。重建后**马上 show()** 时必须用这档：
		// 只 deleteLater 的话，旧控件在真正销毁前仍是子控件，下次 show() 会被一并显示出来。
		DetachNow,
		// 只 deleteLater。可见性由调用方负责。
		DeferOnly,
		// hide + deleteLater，但**不摘离**：控件可能正处在自己的信号处理中（如 anchorClicked）。
		HideThenDefer,
	};

	// 摘掉布局里的全部项并把控件交给事件循环销毁。mode 语义见 ClearMode。
	inline void clearLayout(QLayout* layout, ClearMode mode = ClearMode::DetachNow)
	{
		if (!layout)
			return;

		while (QLayoutItem* item = layout->takeAt(0)) {
			if (QWidget* widget = item->widget()) {
				if (mode != ClearMode::DeferOnly)
					widget->hide();
				if (mode == ClearMode::DetachNow)
					widget->setParent(nullptr);
				widget->deleteLater();
			}
			delete item;
		}
	}
}
