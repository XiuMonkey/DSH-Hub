#pragma once

// 卡片阴影（"悬浮感"）的绘制原语，纯绘制、只依赖 QtGui；颜色不在这里硬编码，调用方从色板取，与「色值只允许出现在 theme-*.json」的约定一致。
// 为什么不用 QGraphicsDropShadowEffect：它每次源控件重绘都要把整棵子树离屏渲染一遍再做模糊（本项目最热的两条路径 —— 流式输出的消息气泡、每敲一个键都要重排的输入卡片 —— 正好都压在这个代价上）；且它的阴影画在控件矩形之外，滚动区与相邻兄弟控件会把边缘裁掉，局部重绘后还容易留残影。
// 算法：N 个同心圆角矩形由外向内叠上去，距离 d 处的累积透光率 = Π(1-a_i)，反解每环 a_i 让结果精确等于目标衰减曲线（两层高斯尾叠加），全程不做任何模糊；结果按（尺寸, 半径, 扩散, 偏移, 颜色, DPR）缓存复用。

#include <QColor>
#include <QMargins>
#include <QRect>

class QPainter;

namespace CardShadow
{
	// 阴影规格（全部是逻辑像素）
	struct Spec
	{
		int blur = 12;    // 扩散范围：阴影从卡片边缘向外延伸多少像素（= 要预留的留白）
		int dy = 3;       // 垂直偏移：正数向下（光从上方来）。只管阴影方向，不管卡片位置
		int radius = 12;  // 卡片圆角；阴影的圆角跟着它同心外扩
	};

	// 浮层档：对齐原版 --dsw-shadow-lv3 的铺开程度 —— 只有这一档算"共用档位"（浮层是独立顶层窗口、留白不占主窗口布局）；贴在窗口里的面各自在调用处写自己的 Spec。
	inline Spec level3() { return Spec{ 24, 8, 14 }; }

	// 画这样一道阴影，四周需要预留多少空白；注意是不对称的：dy > 0（阴影向下）时下侧留得多、上侧留得少。
	QMargins padding(const Spec& spec);

	// 色板颜色字符串 → QColor，必须用它而不是直接 QColor(text)：QColor 的字符串构造只认 #RRGGBB / #AARRGGBB 与 SVG 颜色名，不认 CSS 的 rgba(...)，而色板里的阴影色恰恰写成 rgba(0,0,0,0.45)，直接构造会得到无效 QColor、paint() 因色无效直接返回，现象是"阴影整块消失"且不报任何错。
	QColor parseColor(const QString& text);

	// 在 painter 的坐标系里为 cardRect 这块卡片画阴影：cardRect 必须已按 padding() 从绘制区域内缩，阴影画在 cardRect 之外那段空白之内，所以调用者把阴影画在自己控件的矩形里就永远不会被裁；devicePixelRatio 传控件的 devicePixelRatioF()，高 DPI 才不糊；padOverride 是四周留白的覆盖值（四边都 >= 0 才算给了），用来把"卡片摆哪儿"和"阴影长什么样"分开。
	void paint(QPainter& painter, const QRect& cardRect, const Spec& spec,
		const QColor& color, qreal devicePixelRatio = 1.0,
		const QMargins& padOverride = QMargins(-1, -1, -1, -1));
}
