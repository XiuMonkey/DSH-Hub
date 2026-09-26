#pragma once

// UI 装配样板：清空布局、造按主题着色的滚动区

#include "common/appearance/ThemeManager.h"

#include <QFrame>
#include <QLayout>
#include <QLayoutItem>
#include <QScrollArea>
#include <QWidget>

namespace LayoutUtils
{
	// 三档差异都是有意保留的，别合并
	enum class ClearMode
	{
		// 重建后马上 show() 必须用这档，否则旧控件仍会跟着显示
		DetachNow,
		// 只 deleteLater，可见性由调用方负责
		DeferOnly,
		// 控件可能正处在自己的信号处理中，故不摘离
		HideThenDefer,
	};

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

	// 滚动条在基类构造时已建，那时 objectName 未设、匹配结果被缓存，故须手动 repolish
	inline QScrollArea* makeThemedScrollArea(QWidget* parent, const QString& objectName)
	{
		auto* area = new QScrollArea(parent);
		area->setObjectName(objectName);
		area->setFrameShape(QFrame::NoFrame);
		area->setWidgetResizable(true);
		area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		ThemeManager::instance().repolishScrollArea(area);
		return area;
	}
}
