#include "ui/Tooltip.h"

#include "common/appearance/CardShadow.h"
#include "common/appearance/ThemeManager.h"
#include "core/ConnectionManager.h"
#include "ui/ShadowPanel.h"
#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QMetaObject>
#include <QPainter>
#include <QPointer>
#include <QPropertyAnimation>
#include <QScreen>
#include <QVBoxLayout>
#include <QWidget>

// 悬浮提示：常驻单例顶层无边框窗口，正文自绘，入口只有应用级事件过滤器。四条实测约束：
//   1) 尺寸全自己算，不用 adjustSize()/sizeHint（后者滞后一拍）；
//   2) 折行旗标必须同时带 TextWordWrap 与 TextWrapAnywhere，否则超长串撑爆气泡；
//   3) 改尺寸先钉最小尺寸再 setGeometry()；收起判据是「光标是否还在锚点矩形内」。

namespace
{
	// 必须与 tooltip.qss 里 #tooltipBody 的 border-radius 一致，否则阴影圆角对不上
	constexpr int kRadius = 8;
	constexpr int kPadH = 10;  // 文字到气泡边缘的水平留白
	constexpr int kPadV = 6;  // 垂直留白
	constexpr int kMaxTextWidth = 360;  // 文字折行上限
	constexpr int kGap = 6;  // 气泡与锚点控件之间的间距
	constexpr int kFadeMs = 110;  // 首次淡入时长
	constexpr int kEdgeMargin = 4;  // 贴屏幕边时留的余量
	constexpr int kMeasureHeight = 10000;  // 度量用的"无限高"

	// 两个旗标必须都给：只给 TextWordWrap 时无空格的超长串折不动，气泡被撑到 1300+ 像素；
	// 测量与绘制必须用同一个值，否则量出来的尺寸会漂。
	constexpr int kTextFlags = Qt::TextWordWrap | Qt::TextWrapAnywhere;

	// 与 PopupWindow 同一档：不自己造 Spec，否则"浮起来的东西"各长各的样
	const CardShadow::Spec kShadowSpec = CardShadow::level3();

	QRect frameFor(const QRect& body, const QMargins& pad)
	{
		return body.adjusted(-pad.left(), -pad.top(), pad.right(), pad.bottom());
	}

	// 正文控件：自绘，测量与绘制共用同一套旗标；QLabel 不允许指定折行模式
	class TipText : public QWidget
	{
	public:
		explicit TipText(QWidget* parent = nullptr)
			: QWidget(parent)
		{
			setObjectName(QStringLiteral("tooltipText"));
		}

		void setText(const QString& text)
		{
			m_text = text;
			update();
		}
		QString text() const { return m_text; }

		// 先得到最终宽度再按它算行高，避免"量一行、画两行"把末尾裁掉
		QSize measureWithin(int maxWidth) const
		{
			const QFontMetrics fm(font());
			const int limit = qMax(maxWidth, 1);
			const int natural = fm.boundingRect(
				QRect(0, 0, limit, kMeasureHeight), kTextFlags, m_text).width();
			const int width = qBound(1, natural, limit);
			const int height = fm.boundingRect(
				QRect(0, 0, width, kMeasureHeight), kTextFlags, m_text).height();
			return QSize(width, qMax(height, 1));
		}

	protected:
		void paintEvent(QPaintEvent*) override
		{
			if (m_text.isEmpty())
				return;
			QPainter painter(this);
			painter.setFont(font());
			painter.setPen(palette().color(QPalette::WindowText));
			painter.drawText(rect(), kTextFlags, m_text);
		}

	private:
		QString m_text;
	};

	class TipWindow : public QWidget
	{
	public:
		TipWindow()
			: QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
		{
			setObjectName(QStringLiteral("tooltipWindow"));
			// 圆角之外必须全透明，否则四个角会糊出方块
			setAttribute(Qt::WA_TranslucentBackground);
			setAttribute(Qt::WA_ShowWithoutActivating);
			// 不吃鼠标：一旦吃到 Enter，锚点会收到 Leave，气泡刚冒出来就闪没
			setAttribute(Qt::WA_TransparentForMouseEvents);
			setFocusPolicy(Qt::NoFocus);

			m_body = new QWidget(this);
			m_body->setObjectName(QStringLiteral("tooltipBody"));
			m_body->setAttribute(Qt::WA_StyledBackground, true);

			m_text = new TipText(m_body);

			auto* bodyLayout = new QVBoxLayout(m_body);
			bodyLayout->setContentsMargins(kPadH, kPadV, kPadH, kPadV);
			bodyLayout->setSpacing(0);
			bodyLayout->addWidget(m_text);

			// 阴影画在外壳自己的矩形之内，永远不会被裁（见 CardShadow.h）
			auto* panel = new ShadowPanel(QStringLiteral("shadowFloat"), kShadowSpec, this);
			panel->setRadius(kRadius);
			panel->setCard(m_body);

			auto* outer = new QVBoxLayout(this);
			outer->setContentsMargins(0, 0, 0, 0);
			outer->addWidget(panel);

			m_fade = new QPropertyAnimation(this, "windowOpacity", this);
			m_fade->setDuration(kFadeMs);
			m_fade->setEasingCurve(QEasingCurve::OutCubic);
			m_fade->setStartValue(0.0);
			m_fade->setEndValue(1.0);
			// 动画被打断在中间时收尾也要不透明，否则会留下一个"看得见却以为没显示"的气泡
			dshRegister("Tooltip.001",
				m_fade, &QAbstractAnimation::finished, this, [this]() { setWindowOpacity(1.0); });
		}

		QWidget* owner() const { return m_owner.data(); }
		QString text() const { return m_text->text(); }

		void showFor(QWidget* anchor, const QString& text, const QPoint& cursorGlobal)
		{
			if (!anchor || text.isEmpty()) {
				hideNow();
				return;
			}

			syncTheme();
			watchOwner(anchor);

			const bool wasVisible = isVisible();
			const QSize bodySize = applyText(text);
			const QRect frame = frameRect(anchor, cursorGlobal, bodySize);

			m_fade->stop();

			// 看"上次请求的尺寸"而不是 size()：外壳尺寸在 Windows 上是异步落地的
			const bool sameSize = wasVisible && frame.size() == m_lastFrameSize;
			m_lastFrameSize = frame.size();

			if (sameSize) {
				// 尺寸没变：只挪位置 —— 每次都从 0 重新淡入会闪，像在重播
				move(frame.topLeft());
			}
			else {
				// 直接在**当前可见的**窗口上定几何：隐藏期间改的几何会被丢掉（实测探针轮询 600ms
				// 都没生效），可见窗口上 setGeometry 约 10ms 就到位 —— 这是"长提示切短提示后框还是那么大"的根因
				setGeometry(frame);
			}

			setWindowOpacity(wasVisible ? 1.0 : 0.0);

			show();
			// Qt::ToolTip 已是置顶层，raise() 只是确保它压在其他浮层之上
			raise();

			if (!wasVisible)
				m_fade->start();
		}

		void hideNow()
		{
			m_fade->stop();
			hide();
			setWindowOpacity(1.0); // 复位，下次显示不会以 0 透明度"露个空窗"
			m_text->setText(QString());

			if (m_ownerGoneConn) {
				QObject::disconnect(m_ownerGoneConn);
				m_ownerGoneConn = QMetaObject::Connection();
			}
			m_owner = nullptr;
		}

	private:
		// 常驻单例，而 setMode() 只换全局调色板、不重挂已存在的顶层窗口（只有 reload() 会遍历）
		void syncTheme()
		{
			const int mode = ThemeManager::instance().isDark() ? 1 : 0;
			if (mode == m_themedMode)
				return;
			m_themedMode = mode;
			ThemeManager::instance().applyToWindow(this);
		}

		// 气泡是独立顶层窗口，锚点被删不会自己跟着消失
		void watchOwner(QWidget* anchor)
		{
			if (m_owner == anchor)
				return;
			if (m_ownerGoneConn) {
				QObject::disconnect(m_ownerGoneConn);
				m_ownerGoneConn = QMetaObject::Connection();
			}
			m_owner = anchor;
			if (anchor)
				m_ownerGoneConn = connect(anchor, &QObject::destroyed, this, [this]() { hideNow(); });
		}

		// 文字/本体/外壳三层尺寸全钉死，不交给 adjustSize()：sizeHint 链实测滞后一拍，会忽大忽小、文字被裁
		QSize applyText(const QString& text)
		{
			m_text->setText(text);
			// 字号写在 QSS 里，要 polish 之后 font() 才是最终字体
			m_text->ensurePolished();

			const QSize textSize = m_text->measureWithin(kMaxTextWidth);
			m_text->setFixedSize(textSize);

			const QSize bodySize(textSize.width() + 2 * kPadH, textSize.height() + 2 * kPadV);
			m_body->setFixedSize(bodySize);

			// **先把窗口的最小尺寸改成目标尺寸**再去定几何：刚改完固定尺寸、布局未重新激活时，
			// 窗口最小尺寸仍停在上一个文案，会把"缩小"的请求静默夹回去；显式钉一个下限，夹取即恒等
			const QMargins pad = CardShadow::padding(kShadowSpec);
			setMinimumSize(bodySize.width() + pad.left() + pad.right(),
				bodySize.height() + pad.top() + pad.bottom());

			return bodySize;
		}

		// 摆的是**可见气泡本体**的位置而非外壳窗口：直接摆外壳会平白多出 pad.top() 的偏移
		QRect frameRect(const QWidget* anchor, const QPoint& cursorGlobal, const QSize& bodySize) const
		{
			const QMargins pad = CardShadow::padding(kShadowSpec);
			const QRect anchorGlobal(anchor->mapToGlobal(QPoint(0, 0)), anchor->size());

			QRect body(0, 0, qMax(bodySize.width(), 1), qMax(bodySize.height(), 1));
			body.moveLeft(anchorGlobal.center().x() - body.width() / 2);
			body.moveTop(anchorGlobal.bottom() + 1 + kGap);

			// 用光标所在的屏幕：多显示器下锚点可能在另一块屏上
			QScreen* screen = QGuiApplication::screenAt(cursorGlobal);
			if (!screen)
				screen = QGuiApplication::primaryScreen();
			const QRect avail = screen
				? screen->availableGeometry().adjusted(
					kEdgeMargin, kEdgeMargin, -kEdgeMargin, -kEdgeMargin)
				: QRect();

			QRect frame = frameFor(body, pad);

			// 下面放不下（含阴影留白）就翻到上面；上面也放不下则维持下面，宁可压住锚点也不能一半在屏幕外
			if (avail.isValid() && frame.bottom() > avail.bottom()) {
				QRect above = body;
				above.moveTop(anchorGlobal.top() - 1 - kGap - body.height());
				const QRect aboveFrame = frameFor(above, pad);
				if (aboveFrame.top() >= avail.top())
					frame = aboveFrame;
			}

			if (avail.isValid()) {
				if (frame.right() > avail.right())
					frame.moveRight(avail.right());
				if (frame.left() < avail.left())
					frame.moveLeft(avail.left());
				if (frame.bottom() > avail.bottom())
					frame.moveBottom(avail.bottom());
				if (frame.top() < avail.top())
					frame.moveTop(avail.top());
			}

			return frame;
		}

		QWidget* m_body = nullptr;
		TipText* m_text = nullptr;
		QPropertyAnimation* m_fade = nullptr;
		QPointer<QWidget> m_owner;
		QMetaObject::Connection m_ownerGoneConn;
		// 上次**请求**的外壳尺寸（不是 size()：异步落地，可能停在上一帧）
		QSize m_lastFrameSize;
		int m_themedMode = -1; // -1 未挂过；0 亮色；1 暗色
	};

	// 常驻单例：一次性 new/delete 的话每次悬浮都要重解析样式表（悬浮是高频动作）
	TipWindow* g_tip = nullptr;

	TipWindow* tipWindow()
	{
		if (!g_tip)
			g_tip = new TipWindow;
		return g_tip;
	}

	class TipFilter : public QObject
	{
	public:
		using QObject::QObject;

	protected:
		bool eventFilter(QObject* watched, QEvent* event) override
		{
			const QEvent::Type type = event->type();

			if (type == QEvent::ToolTip) {
				auto* widget = qobject_cast<QWidget*>(watched);
				if (!widget)
					return false;

				const QString text = widget->toolTip();

				// 空文案的事件**必须放行**：ToolTip 沿父子链冒泡，在这里吃掉会把父控件本该显示的提示一起吞了
				if (text.isEmpty()) {
					// 但锚点自己的提示被清掉时要跟着收掉，否则会挂着一个过期的气泡
					if (g_tip && g_tip->isVisible() && widget == g_tip->owner())
						g_tip->hideNow();
					return false;
				}

				tipWindow()->showFor(widget, text, static_cast<QHelpEvent*>(event)->globalPos());
				// 吃掉：不放行的话 Qt 还会再弹一个原生 QTipLabel，两个气泡叠着
				return true;
			}

			// 以下分支只在气泡显示时才有意义；这里刻意不调 tipWindow()，免得为每个事件多加一次调用
			if (!g_tip || !g_tip->isVisible())
				return false;

			switch (type) {
			case QEvent::MouseButtonPress:
			case QEvent::MouseButtonDblClick:
			case QEvent::Wheel:
			case QEvent::KeyPress:
			case QEvent::ApplicationDeactivate:
			case QEvent::WindowDeactivate:
				g_tip->hideNow();
				break;

			case QEvent::Leave:
			case QEvent::Enter:
				// 判据是**光标还在不在锚点上**，而不是事件来源：锚点内部移到自己的子控件也会发 Leave；
				// 而光标从**子控件**直接移出窗口时 Qt 只给子控件发 Leave，只认 watched == owner 会漏掉
				if (!isTipWindow(watched) && !cursorInsideOwner())
					g_tip->hideNow();
				break;

			case QEvent::Hide:
			case QEvent::Close:
				// 锚点自己、或它所在的窗口被藏起来/关掉了
				if (watched == g_tip->owner() || watched == ownerWindow())
					g_tip->hideNow();
				break;

			case QEvent::Move:
			case QEvent::Resize:
			case QEvent::WindowStateChange:
				// 锚点所在窗口被拖动/缩放/最小化时，气泡停在老位置会显得"飘在外面"
				if (watched == ownerWindow())
					g_tip->hideNow();
				break;

			default:
				break;
			}

			return false;
		}

	private:
		// 气泡自己的窗口（含其内部部件）。它的进出不参与"离开锚点"的判断
		static bool isTipWindow(QObject* watched)
		{
			auto* widget = qobject_cast<QWidget*>(watched);
			return widget && g_tip && (widget == g_tip || g_tip->isAncestorOf(widget));
		}

		static bool cursorInsideOwner()
		{
			QWidget* owner = g_tip ? g_tip->owner() : nullptr;
			if (!owner)
				return false;
			return owner->rect().contains(owner->mapFromGlobal(QCursor::pos()));
		}

		static QWidget* ownerWindow()
		{
			QWidget* owner = g_tip ? g_tip->owner() : nullptr;
			return owner ? owner->window() : nullptr;
		}
	};
}

namespace Tooltip
{
	void install()
	{
		static bool installed = false;
		if (installed || !qApp)
			return;
		installed = true;
		// 挂在 qApp 上：ToolTip 是发给控件的，只有应用级过滤器能统一看到
		qApp->installEventFilter(new TipFilter(qApp));
	}
}
