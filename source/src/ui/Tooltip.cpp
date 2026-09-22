#include "ui/Tooltip.h"

#include "common/appearance/CardShadow.h"
#include "ui/ShadowPanel.h"
#include "common/appearance/ThemeManager.h"

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

// ------------------------------------------------------------------
// Tooltip.cpp
// ------------------------------------------------------------------
// 见头文件。这里只有三块：
//   1) TipText / TipWindow —— 气泡本体（常驻单例的顶层无边框窗口，正文自绘）
//   2) TipFilter           —— 应用级事件过滤器，唯一的外部入口
//   3) 三个公开函数         —— install() / hide() / 测试用的查询
//
// 四条改之前必须知道的约定（都是实测踩出来的，注释里各有展开）：
//   * 尺寸**全部自己算**，不碰 adjustSize()/sizeHint —— 它们在这个窗口上给出的是
//     滞后一拍的尺寸，是"气泡忽大忽小"的根因；
//   * 折行旗标必须同时带 TextWordWrap 与 TextWrapAnywhere —— 否则没有空格的超长串
//     （参数 JSON、Windows 长路径）折不动，能把气泡撑到 1300+ 像素宽；
//   * 改尺寸时**先把窗口最小尺寸钉成目标尺寸**，再在可见窗口上 setGeometry() ——
//     前者挡住夹取，后者避免"隐藏期间改几何被丢掉"；
//   * 收起判据用「光标是否还在锚点矩形内」，不能比较 watched == owner ——
//     光标从锚点的子控件直接移出窗口时，锚点收不到 Leave。
// ------------------------------------------------------------------

namespace
{
	// ------------------------------------------------------------------
	// 观感常量
	// ------------------------------------------------------------------
	// 圆角必须与 tooltip.qss 里 #tooltipBody 的 border-radius 一致，
	// 否则 ShadowPanel 画的阴影圆角会和本体对不上。
	constexpr int kRadius = 8;
	constexpr int kPadH = 10;          // 文字到气泡边缘的水平留白
	constexpr int kPadV = 6;           // 垂直留白
	constexpr int kMaxTextWidth = 360; // 文字折行上限（工具描述 + 参数 JSON 会很长）
	constexpr int kGap = 6;            // 可见气泡与锚点控件之间的间距
	constexpr int kFadeMs = 110;       // 首次出现的淡入时长
	constexpr int kEdgeMargin = 4;     // 贴屏幕边时留的余量
	constexpr int kMeasureHeight = 10000; // 度量用的"无限高"

	// 折行旗标。**两个必须都给**，这是实测结论：
	//   * 只给 TextWordWrap：按词边界折行，遇到没有空格的超长串（参数 JSON、
	//     Windows 长路径、长英文标识符）折不动，整串顶出去 —— 实测
	//     boundingRect(@360, TextWordWrap, "A"×152) 返回宽 1283，
	//     气泡会被撑到 1351 像素；
	//   * 两个都给：超长串被断到 355（≤ 上限），而普通英文的换行结果与纯
	//     TextWordWrap **完全一致**（词边界仍然优先，不会被劈开）。
	// 注意测量与绘制必须用同一个值，否则量出来的尺寸和实际渲染会漂。
	constexpr int kTextFlags = Qt::TextWordWrap | Qt::TextWrapAnywhere;

	// 浮层阴影档：与 PopupWindow 同一档（CardShadow 里唯一被定义为"共用档位"的那个）。
	// 不自己造 Spec —— 浮层的留白不占布局，就该用给足的那一档；单独造一个只会让
	// "浮起来的东西"各长各的样。
	const CardShadow::Spec kShadowSpec = CardShadow::level3();

	// 外壳矩形 = 可见气泡本体向外长一圈阴影留白。
	// 注意 QRect::adjusted 是加在四条边上，所以左右传 -left/+right 才是"变大"。
	QRect frameFor(const QRect& body, const QMargins& pad)
	{
		return body.adjusted(-pad.left(), -pad.top(), pad.right(), pad.bottom());
	}

	// ------------------------------------------------------------------
	// 正文控件：自绘，不是 QLabel
	// ------------------------------------------------------------------
	// 为什么不用 QLabel（两条，都是实测踩出来的）：
	//   1) QLabel 开 wordWrap 后**只按词边界折行**，没有空格的超长串折不动，
	//      量出来的宽度可以远大于折行上限 —— 气泡要么被撑成一条长带，要么把字裁掉；
	//      而 QLabel 不允许指定折行模式（内部固定用 WordWrap）；
	//   2) 自绘能让"量尺寸"与"画文字"共用同一套旗标，两者永远不会漂。
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

		// 在 maxWidth 以内的排版尺寸：宽取折行后最长行，高**在该宽下重新量**。
		// 分两步是有意的：先得到最终宽度，再按这个宽度算行高，量出来的高就等于
		// 渲染时的行数，不会出现"量一行、画两行"把末尾裁掉。
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
			// 颜色来自 tooltip.qss 的 #tooltipText { color: ... }
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
			// 半透明底：圆角之外必须是全透明，否则四个角会糊出方块
			setAttribute(Qt::WA_TranslucentBackground);
			// 显示时不抢焦点（悬浮提示永远不该打断输入）
			setAttribute(Qt::WA_ShowWithoutActivating);
			// 不吃鼠标：气泡就浮在光标旁边，指针扫过它不能被它挡住。
			// 更要紧的是它一旦吃到 Enter，底下的锚点控件就会收到 Leave，
			// 我们自己的隐藏逻辑会把气泡收掉 —— 观感是"气泡刚冒出来就闪没"。
			setAttribute(Qt::WA_TransparentForMouseEvents);
			setFocusPolicy(Qt::NoFocus);

			// 圆角卡片本体（背景/描边/圆角都在 tooltip.qss 里）
			m_body = new QWidget(this);
			m_body->setObjectName(QStringLiteral("tooltipBody"));
			m_body->setAttribute(Qt::WA_StyledBackground, true);

			m_text = new TipText(m_body);

			auto* bodyLayout = new QVBoxLayout(m_body);
			bodyLayout->setContentsMargins(kPadH, kPadV, kPadH, kPadV);
			bodyLayout->setSpacing(0);
			bodyLayout->addWidget(m_text);

			// 浮层外壳：阴影画在外壳自己的矩形之内，永远不会被裁（见 CardShadow.h）
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
			// 兜底：万一动画被打断在中间，收尾也保证是完全不透明，
			// 否则会留下一个"看得见却以为没显示"的半透明气泡
			connect(m_fade, &QAbstractAnimation::finished, this, [this]() { setWindowOpacity(1.0); });
		}

		QWidget* owner() const { return m_owner.data(); }
		QString text() const { return m_text->text(); }

		// 显示（或就地更新）气泡
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

			// 是否只需要挪位置：看"上次请求的尺寸"而不是 size() —— 外壳尺寸在 Windows
			// 上是异步落地的，读 size() 可能还停在上一帧。
			const bool sameSize = wasVisible && frame.size() == m_lastFrameSize;
			m_lastFrameSize = frame.size();

			if (sameSize) {
				// 尺寸没变（多半是从一个按钮移到了另一个按钮）：只挪位置。
				// 每次都从 0 重新淡入会闪，像在重播。
				move(frame.topLeft());
			}
			else {
				// 尺寸变了：直接在**当前可见的**窗口上定几何。
				//
				// 别想着"先 hide() 再 setGeometry() 再 show() 更稳妥" —— 实测正好相反：
				// 隐藏期间改的几何会被丢掉（探针轮询 600ms 都没生效，气泡就一直用着
				// 上一个文案的尺寸），而在可见窗口上 setGeometry 约 10ms 就到位。
				// 这是"长提示切短提示后框还是那么大"的直接原因。
				setGeometry(frame);
			}

			// 已经在屏幕上就保持不透明（只挪位置/换尺寸，不重播淡入）
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
		// 深浅色变了就重挂样式表。
		// 本窗口是常驻单例（不像各弹窗每次新建），而 ThemeManager::instance().setMode() 只换全局调色板、
		// 不会重挂已存在的顶层窗口（只有 reload() 会遍历），所以必须自己盯。
		void syncTheme()
		{
			const int mode = ThemeManager::instance().isDark() ? 1 : 0;
			if (mode == m_themedMode)
				return;
			m_themedMode = mode;
			ThemeManager::instance().applyToWindow(this);
		}

		// 锚点被删时立刻收掉：气泡是独立顶层窗口，不会随锚点一起消失
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

		// 重新排版文案，返回**可见本体**该有的尺寸。
		//
		// 这里刻意把文字、本体、外壳三层尺寸全部钉死，而不是 setText 完交给
		// adjustSize()：
		//   * adjustSize() 读的是 sizeHint 链（外层布局 -> ShadowPanel -> 本体 ->
		//     文字），这条链在实测里会给出滞后一拍的尺寸 —— 于是窗口和内部对不上，
		//     气泡忽大忽小、文字还会溢出被裁；
		//   * 尺寸全固定之后，布局只负责把文字摆到 (kPadH, kPadV)，不再参与"算多大"。
		QSize applyText(const QString& text)
		{
			m_text->setText(text);
			// 先让样式表落地：字号写在 QSS 里，要 polish 之后 font() 才是最终字体，
			// 早于此度量出来的宽高会和实际渲染对不上。
			m_text->ensurePolished();

			const QSize textSize = m_text->measureWithin(kMaxTextWidth);
			m_text->setFixedSize(textSize);

			const QSize bodySize(textSize.width() + 2 * kPadH, textSize.height() + 2 * kPadV);
			m_body->setFixedSize(bodySize);

			// 关键一步：**先把窗口的最小尺寸改成目标尺寸**，再去定几何。
			//
			// 原因（实测踩了很久）：QWidget 定尺寸时会把目标夹进 [minimumSize,
			// maximumSize] 里，而顶层窗口的最小尺寸是从布局推导出来的 minimumSizeHint。
			// 本函数刚改完文字/本体的固定尺寸，布局还没重新激活，窗口的最小尺寸此刻仍
			// 停在**上一个文案**的大小 —— 于是"缩小"的请求被静默夹回去，气泡就一直
			// 挂着上一个文案的框（长提示切到短提示后尤其明显，就是"有时过大"）。
			// 显式钉一个最小尺寸（它优先于推导值），夹取就变成了恒等。
			const QMargins pad = CardShadow::padding(kShadowSpec);
			setMinimumSize(bodySize.width() + pad.left() + pad.right(),
				bodySize.height() + pad.top() + pad.bottom());

			return bodySize;
		}

		// 算出外壳窗口该摆的矩形。摆放算的是**可见气泡本体**的位置，而不是外壳窗口的
		// 位置：直接摆外壳会平白多出 pad.top() 的偏移，间距就不准了（尤其翻到上方时，
		// 偏差是两倍的 pad）。
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

			// 下面放不下（含阴影留白）就翻到上面；上面也放不下就维持下面，
			// 交给后面的夹取 —— 宁可压住锚点也不能一半在屏幕外。
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

	// 常驻单例。父对象留空是有讲究的：
	//   * 挂到主窗口上 —— 切主题会换掉主窗口，气泡会被一起删；
	//   * 一次性 new/delete —— 每次悬浮都要重解析一遍样式表，而悬浮是高频动作。
	// 所以就用一个活到进程结束的单例（和 Qt 自己 QTipLabel 的做法一致）。
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

				// 空文案的事件**必须放行**。Qt 的 QEvent::ToolTip 是沿父子链冒泡的
				// （实测：鼠标停在没提示的子控件上时，事件先给子控件、再给带提示的
				// 父控件）。在这里吃掉，等于把父控件本该显示的提示一起吞了。
				if (text.isEmpty()) {
					// 但锚点自己的提示被清掉时要跟着收掉（例如 SessionStatsLine 变宽后
					// 不再截断、把 tooltip 清空），否则会挂着一个过期的气泡。
					if (g_tip && g_tip->isVisible() && widget == g_tip->owner())
						g_tip->hideNow();
					return false;
				}

				tipWindow()->showFor(widget, text, static_cast<QHelpEvent*>(event)->globalPos());
				// 吃掉：不放行的话 Qt 还会再弹一个原生 QTipLabel，变成两个气泡叠着
				return true;
			}

			// 以下分支只在气泡显示时才有意义。这里刻意不调 tipWindow()：
			// 没悬浮过就别为此建窗口，也免得给每个事件加一次函数调用。
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
				// 判据是**光标还在不在锚点上**，而不是"这个事件是不是锚点发的"。
				// 两个方向都踩过：
				//   * 锚点内部从自己移到自己的子控件也会发 Leave（那不是离开），
				//     按事件来源判会误收；
				//   * 反过来，光标从锚点的**子控件**直接移出窗口时，Qt 只给子控件发
				//     Leave，锚点早就收过自己的 Leave 了 —— 只认 watched == owner
				//     就会漏掉，气泡永远挂着（提示挂在带子控件的容器上时必现）。
				// 看光标位置这两种都兜得住。
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
		// 气泡自己的窗口（含其内部部件）。它的进出不参与"离开锚点"的判断 ——
		// 虽然它透明不吃鼠标，但万一平台没做到，也不能因为它的 Enter 把自己收掉。
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
		// 挂在 qApp 上：QEvent::ToolTip 是发给控件的，只有应用级过滤器能统一看到
		qApp->installEventFilter(new TipFilter(qApp));
	}
}