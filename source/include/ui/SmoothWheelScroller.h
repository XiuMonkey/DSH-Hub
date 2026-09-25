#pragma once

// 鼠标滚轮的"平滑化"：接管某个 QAbstractScrollArea 视口上的滚轮事件，把一格换算成像素增量累积成
// 目标位置，再用 QPropertyAnimation 对滚动条 value 做补间。
// 为什么需要：Qt Widgets 默认的滚轮处理是一次同步 setValue（Qt 6.11.2 实测一格 =
// wheelScrollLines 3 × singleStep 20 = 60px，中间没有任何过渡帧），观感就是"跳格"。
// 宿主必须靠 userScrolledAway / isAnimating 判断"用户正在滚"：流式输出的"离底 80px 内跟随底部"
// 比一格 60px 还大，只看位置会把用户往上滚的一格立刻拽回底部。

#include <QObject>

class QAbstractScrollArea;
class QPropertyAnimation;
class QWheelEvent;

class SmoothWheelScroller : public QObject
{
	Q_OBJECT

public:
	// area 为空（或稍后销毁）时本对象只是空转，不会崩；只接管滚轮，键盘、拖动滚动条、程序 setValue
	// 都不受影响，滚动区自己没得滚时不接管（嵌套滚动链照旧）
	explicit SmoothWheelScroller(QAbstractScrollArea* area, QObject* parent = nullptr);
	~SmoothWheelScroller() override;

	// 宿主自己要把滚动位置钉到某处前必须先调用，否则两者打架
	void stop();
	// 补间是否正在进行：宿主据此判断"用户正在滚"，期间不要抢滚动位置
	bool isAnimating() const;

signals:
	// 用户主动往上滚（想看更早的内容）；同步发出 —— 滚轮事件处理完就已发出而补间还没走第一步、
	// 滚动条仍贴底，光看位置看不出来
	void userScrolledAway();

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	// 处理视口上的一次滚轮；返回 true = 已被接管（事件不再往下走）
	bool handleWheel(QWheelEvent* event);
	// 按当前 m_target 启动 / 重定向补间
	void animateToTarget();
	// 把目标位置同步成滚动条当前值（补间结束时、被外部打断时用）
	void syncTargetToBar();

	QAbstractScrollArea* m_area = nullptr;
	QPropertyAnimation* m_animation = nullptr;

	// 用 double：高精度设备（触控板）给的是小增量，取整会把它磨没
	double m_target = 0.0;

	// 最近一段补间的意图：移到底部边界（用于"底部还在长高时继续追"），以及用户最近一次是不是在往下滚
	bool m_aimedAtBottom = false;
	bool m_lastDeltaDown = false;
};
