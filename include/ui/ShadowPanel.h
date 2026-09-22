#pragma once

// ------------------------------------------------------------------
// ShadowPanel.h
// ------------------------------------------------------------------
// 给任意卡片控件套一层「悬浮外壳」，用来加阴影（悬浮感），而不动卡片自己。
//
// 分工：
//   · 外壳透明，只在四周预留的空白里画一圈阴影（用 CardShadow）；
//   · 被包的卡片保持原样 —— QSS 里的背景、圆角、边框一条都不用改，
//     objectName 与选择器继续生效；
//   · 阴影色从色板取（构造时给 key），每次绘制现取，所以换主题后重绘即自动跟随，
//     不需要额外的通知机制。
//
// 为什么要有这一层：QSS 不支持 box-shadow，而 Qt 的 QGraphicsDropShadowEffect
// 的阴影画在控件矩形之外，会被滚动区/相邻兄弟控件裁掉并留下残影（详见 CardShadow.h）。
// 让阴影画在「外壳自己的矩形之内」是唯一到处都稳的做法。

#include "common/appearance/CardShadow.h"

#include <QString>
#include <QWidget>

class QPaintEvent;
class QVBoxLayout;

class ShadowPanel : public QWidget
{
	Q_OBJECT

public:
	// shadowKey：色板里的阴影色 key（"shadow" / "shadowSubtle" / "shadowFloat"）
	// spec     ：阴影规格。**必须显式给**：各面的留白成本不一样（浮层不占布局、
	//            贴边的面板要从布局里切），用默认值容易悄悄用错档
	ShadowPanel(const QString& shadowKey, const CardShadow::Spec& spec,
		QWidget* parent = nullptr);

	// 把卡片交给外壳：立刻纳入内部布局，四周留出有效留白对应的空白
	void setCard(QWidget* card);

	// 卡片圆角跟卡片自己的 QSS 一致，阴影形状才对得上
	void setRadius(int radius);

	// 覆盖外壳四周的留白 —— 也就是"卡片在外壳里摆哪儿"。
	// 四边都 >= 0 才生效；传 QMargins(-1,-1,-1,-1) 恢复"按 spec 推导"。
	//
	// 用途：只把卡片在外壳里挪个位置（例如整体下移一段），而阴影的方向（spec.dy）
	// 与形状都不变。要让外壳外面的布局不受影响，覆盖值的四边之和
	// 得和 padding(spec) 保持一致（总高不变）。
	void setPadding(const QMargins& pad);

protected:
	void paintEvent(QPaintEvent* event) override;

private:
	// 把有效留白落到内部布局的边距上
	void applyMargins();
	// 实际生效的四周留白：给了覆盖值就用它，否则按 spec 推导
	QMargins effectivePadding() const;

	CardShadow::Spec m_spec;
	QString m_shadowKey;
	// 四边都为负 = 没有覆盖
	QMargins m_padOverride{ -1, -1, -1, -1 };
	QWidget* m_card = nullptr;
	QVBoxLayout* m_layout = nullptr;
};
