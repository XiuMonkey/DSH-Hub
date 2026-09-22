#pragma once

// 应用统一的「悬浮提示」：应用级事件过滤器拦 QEvent::ToolTip，接管 Qt 原生 QToolTip，换成主题一致的自绘气泡。
// 为什么自绘：原生提示框是方角纯色底 + 系统默认字体，在自绘圆角窗口里很突兀，而且 QSS 管不了它（QToolTip 是 Qt 内部建的顶层 QLabel，圆角/描边/阴影/内外边距一概不生效）。
// 要点：文案唯一来源仍是 QWidget::toolTip()（既有 setToolTip 调用点零改动），延时**由 Qt 给**、不再叠加自定义延时；气泡窗口是常驻单例，显示前会检查深浅色是否变过。

#include <QString>

class QWidget;

namespace Tooltip
{
	// 装应用级事件过滤器：QApplication 构造之后、任意窗口显示之前调一次即可，重复调用无副作用
	void install();
}
