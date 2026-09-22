#pragma once

// ------------------------------------------------------------------
// SmoothWheelScroller.h
// ------------------------------------------------------------------
// 鼠标滚轮的"平滑化"：接管某个 QAbstractScrollArea 视口上的滚轮事件，把一格换算成
// 像素增量、累积成一个目标位置，再用 QPropertyAnimation 对滚动条的 value 做补间，
// 让一格滚轮变成"十几帧推过去"而不是"一次跳到位"。
//
// 为什么需要这一层：
//   Qt Widgets 默认的滚轮处理是一次同步 setValue —— 本机实测（Qt 6.11.2）：
//   一格（angleDelta 120）= QApplication::wheelScrollLines(3) × 滚动条 singleStep(20)
//   = 60px，中间没有任何过渡帧，观感就是"跳格"。主窗口的 chatScrollArea 从建起来
//   就没有任何滚轮处理，走的正是这条默认路径。
//
// 宿主还需要知道"用户正在滚"：
//   流式输出时宿主会"跟随底部"，而它原来的判定是"离底 80px 内"——比一格滚轮(60px)
//   还大，于是用户往上滚一格会被下一帧立刻拽回底部。滚轮事件是**同步**启动补间的，
//   而那一帧可能赶在补间第一个步进之前（此时滚动条还没离开底部），所以光看位置不够：
//   宿主必须能拿到"用户要离开底部"的同步信号（userScrolledAway）与"正在滚"的状态
//   （isAnimating）；见 MessageHost::flushStreamingFrame。
//
// 用法：装在任意滚动区上即可，不需要换控件类型：
//     new SmoothWheelScroller(scrollArea, parent);   // 只接管滚轮
// 键盘、拖动滚动条、程序 setValue 都不受影响；滚动区自己没得滚时不接管，
// 事件照旧往上层的滚动区传（嵌套滚动链不变）。
// ------------------------------------------------------------------

#include <QObject>

class QAbstractScrollArea;
class QPropertyAnimation;
class QWheelEvent;

class SmoothWheelScroller : public QObject
{
	Q_OBJECT

public:
	/** area 为空（或稍后被销毁）时本对象只是空转，不会崩。 */
	explicit SmoothWheelScroller(QAbstractScrollArea* area, QObject* parent = nullptr);
	~SmoothWheelScroller() override;

	/** 立刻结束进行中的补间（宿主自己要把滚动位置钉到某处前必须先调用，否则两者打架）。 */
	void stop();
	/** 补间是否正在进行：宿主据此判断"用户正在滚"，期间不要抢滚动位置。 */
	bool isAnimating() const;

signals:
	/**
	 * 用户主动往上滚（想看更早的内容）。**同步发出**：滚轮事件处理完就已经发出，
	 * 而这时补间还没走第一步，滚动条仍贴着底部 —— 宿主只有靠这个信号才能在
	 * 那一瞬间知道"用户要离开底部了"，光看位置是看不出来的。
	 */
	void userScrolledAway();

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	/** 处理视口上的一次滚轮；返回 true = 已被接管（事件不再往下走）。 */
	bool handleWheel(QWheelEvent* event);
	/** 按当前 m_target 启动 / 重定向补间。 */
	void animateToTarget();
	/** 把目标位置同步成滚动条当前值（补间结束时、被外部打断时用）。 */
	void syncTargetToBar();

	QAbstractScrollArea* m_area = nullptr;
	QPropertyAnimation* m_animation = nullptr;

	// 目标滚动位置用 double：高精度设备（触控板）给的是小增量，取整会把它磨没
	double m_target = 0.0;

	// 最近一段补间的意图：往下滚到边界（用于"底部还在长高时继续追"），以及
	// 用户最近一次是不是在往下滚（流式输出期间内容持续变高，底部会移动）
	bool m_aimedAtBottom = false;
	bool m_lastDeltaDown = false;
};
