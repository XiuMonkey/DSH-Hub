#pragma once

// UI 装配样板：清空布局、造按主题着色的滚动区这类各处重复的写法。

#include "common/appearance/ThemeManager.h"

#include <QFrame>
#include <QLayout>
#include <QLayoutItem>
#include <QScrollArea>
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

	// 造一个按主题着色的 QScrollArea。
	//
	// ⚠️ 这里必须自己调 repolishScrollArea：滚动条是 QAbstractScrollArea 基类构造时建的，
	// 那时 objectName 还没设，QStyleSheetStyle 会把"匹配不到规则"缓存下来，之后再设名字也不会
	// 重新匹配（详见 ThemeManager.h 的说明）。所以设完名字就得补一次。
	//
	// 调用方按需自行追加：setMinimumHeight、setFocusPolicy(NoFocus)（弹层里用，防止可聚焦
	// 子控件把弹层关掉）、setWidget(…) 之后的 setAlignment 等。
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
