#pragma once

// 卡片阴影绘制原语，纯绘制、只依赖 QtGui；颜色由调用方从色板取。
// 不用 QGraphicsDropShadowEffect：每次源控件重绘都要离屏渲染整棵子树再模糊，阴影还画在控件矩形外会被裁掉。
// 算法：N 个同心圆角矩形由外向内叠加、不模糊，结果按（尺寸, 半径, 扩散, 偏移, 颜色, DPR）缓存。

#include <QColor>
#include <QMargins>
#include <QRect>

class QPainter;

namespace CardShadow
{
	struct Spec
	{
		int blur = 12; // 阴影从卡片边缘向外延伸的像素数
		int dy = 3; // 垂直偏移，正数向下
		int radius = 12; // 卡片圆角；阴影圆角同心外扩
	};

	// 浮层档：对齐原版 --dsw-shadow-lv3；只有浮层算共用档位
	inline Spec level3() { return Spec{ 24, 8, 14 }; }

	// 四周需预留的留白；不对称，dy > 0 时下侧留得多
	QMargins padding(const Spec& spec);

	// 必须用它：色板的 rgba(...) QColor 字符串构造不认 → 无效色 → 阴影不画且不报错
	QColor parseColor(const QString& text);

	// cardRect 须已按 padding() 内缩，阴影画在它之外的空白；padOverride 四边 >= 0 才算给了
	void paint(QPainter& painter, const QRect& cardRect, const Spec& spec,
		const QColor& color, qreal devicePixelRatio = 1.0, const QMargins& padOverride = QMargins(-1, -1, -1, -1));
}
