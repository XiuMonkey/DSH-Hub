// 无边框窗口的窗口级功能逻辑（见 WindowFrame.h）：只有判定与平台调用，没有绘制代码。
// 唯一外部依赖是 UiStage（遮罩范围、窗口条命中测试）。

#include "common/appearance/WindowFrame.h"
#include "ExtensionSystem/UiStage.h"

#include <QByteArray>
#include <QHash>
#include <QLayout>
#include <QPointer>
#include <QSet>
#include <QStyle>
#include <QWidget>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <dwmapi.h>
#  pragma comment(lib, "dwmapi.lib")

// 老 SDK 的 dwmapi.h 缺这两个常量
#  ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#    define DWMWA_WINDOW_CORNER_PREFERENCE 33
#  endif
#  ifndef DWMWCP_DONOTROUND
#    define DWMWCP_DONOTROUND 1
#  endif
#endif

namespace
{
	constexpr int kResizeBorder = 5;

	const char kTitleBarObjectName[] = "windowTitleBar";
	const char kWindowControlProperty[] = "dshWindowControl";
	const char kMaximizedProperty[] = "maximized";

	bool isWindowControlAt(QWidget* window, const QPoint& clientPos)
	{
		for (QWidget* w = window->childAt(clientPos); w && w != window; w = w->parentWidget()) {
			if (w->property(kWindowControlProperty).toBool())
				return true;
		}
		return false;
	}

	// pos 是**逻辑**客户区坐标
	qintptr hitTest(QWidget* window, const QWidget* titleBar, const QPoint& pos)
	{
#ifdef Q_OS_WIN
		// 四周边缘 = 缩放热区（最大化/全屏时不可缩放）
		if (!window->isMaximized() && !window->isFullScreen()) {
			const QRect client = window->rect();
			const bool left = pos.x() < kResizeBorder;
			const bool right = pos.x() >= client.width() - kResizeBorder;
			const bool top = pos.y() < kResizeBorder;
			const bool bottom = pos.y() >= client.height() - kResizeBorder;
			if (top && left)
				return HTTOPLEFT;
			if (top && right)
				return HTTOPRIGHT;
			if (bottom && left)
				return HTBOTTOMLEFT;
			if (bottom && right)
				return HTBOTTOMRIGHT;
			if (left)
				return HTLEFT;
			if (right)
				return HTRIGHT;
			if (top)
				return HTTOP;
			if (bottom)
				return HTBOTTOM;
		}

		// 自绘标题栏交给系统当标题栏，但按钮区域必须排除，否则按钮收不到鼠标事件
		if (titleBar && titleBar->isVisible()) {
			const QRect titleRect(titleBar->mapTo(window, QPoint(0, 0)), titleBar->size());
			if (titleRect.contains(pos) && !isWindowControlAt(window, pos))
				return HTCAPTION;
		}

		// 架空时原生标题栏被藏起来，回落到扩展登记的自绘窗口条，否则扩展自绘的界面拖不动
		int captionTop = 0;
		int captionHeight = 0;
		if (UiStage::captionBand(window, &captionTop, &captionHeight)) {
			const QRect band(0, captionTop, window->width(), captionHeight);
			if (band.contains(pos) && !isWindowControlAt(window, pos))
				return HTCAPTION;
		}
#else
		Q_UNUSED(window);
		Q_UNUSED(titleBar);
		Q_UNUSED(pos);
#endif
		return HTCLIENT;
	}

	const char kScrimObjectName[] = "windowScrim";

	struct OverlayState
	{
		// 遮罩是宿主的子控件，Qt 会一起删；这里只留弱引用
		QPointer<QWidget> widget;
		QSet<const void*> owners;   // 谁在用（按调用方 this 记名）
	};

	QHash<const QWidget*, OverlayState>& overlayStates()
	{
		static QHash<const QWidget*, OverlayState> states;
		return states;
	}

	QWidget* ensureOverlayWidget(QWidget* host)
	{
		if (!host)
			return nullptr;

		OverlayState& state = overlayStates()[host];
		if (state.widget)
			return state.widget;

		auto* scrim = new QWidget(host);
		scrim->setObjectName(QLatin1String(kScrimObjectName));
		scrim->setAttribute(Qt::WA_StyledBackground, true);
		state.widget = scrim;

		// 连接挂在 sender 上，宿主没了连接自然断掉
		QObject::connect(host, &QObject::destroyed, [host]() { overlayStates().remove(host); });

		return scrim;
	}
}

namespace WindowFrame
{
	void applyNativeStyle(QWidget* window)
	{
#ifdef Q_OS_WIN
		if (!window)
			return;

		HWND hwnd = reinterpret_cast<HWND>(window->winId());
		if (!hwnd)
			return;

		LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
		// 幂等：重复 SetWindowPos 会打断最大化动画
		if ((style & (WS_THICKFRAME | WS_CAPTION)) == (WS_THICKFRAME | WS_CAPTION))
			return;

		// FramelessWindowHint 会摘掉 WS_CAPTION/WS_THICKFRAME，这里补回来才能保留投影与贴边吸附
		style |= WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
		SetWindowLongPtrW(hwnd, GWL_STYLE, style);

		// Win11 默认的 8px 系统圆角会裁掉自绘 12px 描边的四角，故关掉只留投影
		const BOOL doNotRound = DWMWCP_DONOTROUND;
		DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &doNotRound, sizeof(doNotRound));

		SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#else
		Q_UNUSED(window);
#endif
	}

	bool handleNativeMessage(QWidget* window, const QWidget* titleBar,
		const QByteArray& eventType, void* message, qintptr* result)
	{
#ifdef Q_OS_WIN
		if (!window || !result)
			return false;
		if (eventType != QByteArrayLiteral("windows_generic_MSG")
			&& eventType != QByteArrayLiteral("windows_dispatcher_MSG")) {
			return false;
		}

		MSG* msg = static_cast<MSG*>(message);
		if (!msg)
			return false;

		switch (msg->message) {
		case WM_NCCALCSIZE:
			if (msg->wParam == TRUE) {
				// 返回 0：客户区 = 整个窗口。这个等式必须"永远"成立：最大化时若把客户区钉到
				// 工作区，Qt 的重绘目标就落到可视区之外、窗口表面再也不更新
				*result = 0;
				return true;
			}
			break;
		case WM_NCHITTEST: {
			// WM_NCHITTEST 是物理像素、Qt 几何是逻辑像素，必须先换算，否则整块窗口都成了缩放热区
			const POINTS point = MAKEPOINTS(msg->lParam);
			POINT nativePoint{ point.x, point.y };
			ScreenToClient(msg->hwnd, &nativePoint);
			const qreal scale = window->devicePixelRatioF() > 0.0 ? window->devicePixelRatioF() : 1.0;
			const QPoint pos(qRound(nativePoint.x / scale), qRound(nativePoint.y / scale));
			*result = hitTest(window, titleBar, pos);
			return true;
		}
		default:
			break;
		}
#else
		Q_UNUSED(window);
		Q_UNUSED(titleBar);
		Q_UNUSED(eventType);
		Q_UNUSED(message);
		Q_UNUSED(result);
#endif
		return false;
	}

	bool isEdgeToEdge(const QWidget* window)
	{
		return window && (window->isMaximized() || window->isFullScreen());
	}

	void applyBorderState(QWidget* surface, bool edgeToEdge)
	{
		if (!surface)
			return;
		if (surface->property(kMaximizedProperty).toBool() == edgeToEdge)
			return;

		surface->setProperty(kMaximizedProperty, edgeToEdge);
		if (QStyle* style = surface->style()) {
			style->unpolish(surface);
			style->polish(surface);
		}
		surface->update();
	}

	// 最大化矩形比工作区多一圈不可见缩放边框；客户区不能再动、带 WS_MAXIMIZE 的窗口又没法
	// 用 SetWindowPos 挪，故在内容层补回来，让可见内容正好落进工作区
	void applyMaximizedContentInset(const QWidget* window, QLayout* contentLayout)
	{
		if (!window || !contentLayout)
			return;

		int inset = 0;
#ifdef Q_OS_WIN
		if (window->isMaximized() && !window->isFullScreen()) {
			if (HWND hwnd = reinterpret_cast<HWND>(window->winId())) {
				RECT windowRect{};
				MONITORINFO info{};
				info.cbSize = sizeof(info);
				if (GetWindowRect(hwnd, &windowRect)
					&& GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
					const int overflow = qMin(qMin(info.rcWork.left - static_cast<int>(windowRect.left),
						info.rcWork.top - static_cast<int>(windowRect.top)),
						qMin(static_cast<int>(windowRect.right) - info.rcWork.right,
							static_cast<int>(windowRect.bottom) - info.rcWork.bottom));
					const qreal scale = window->devicePixelRatioF() > 0.0 ? window->devicePixelRatioF() : 1.0;
					inset = qMax(0, qRound(overflow / scale));
				}
			}
		}
#endif

		const QMargins margins(inset, inset, inset, inset);
		if (contentLayout->contentsMargins() != margins)
			contentLayout->setContentsMargins(margins);
	}

	// 返回 edgeToEdge，供调用方刷新标题栏图标
	bool applyFrameStyle(QWidget* window, QWidget* surface)
	{
		if (!window || !surface)
			return false;

		const bool edgeToEdge = isEdgeToEdge(window);
		applyBorderState(surface, edgeToEdge);
		applyMaximizedContentInset(window, surface->layout());
		return edgeToEdge;
	}

	QRect overlayRect(const QWidget* host)
	{
		if (!host)
			return QRect();

		QRect area = host->rect();

		// 架空时整块客户区都是扩展的画布：遮罩要铺满，否则顶部会留一条没被压暗的缝
		if (UiStage::isTakenOver(host))
			return area;

		const QWidget* bar = host->findChild<QWidget*>(QLatin1String(kTitleBarObjectName));
		if (bar) {
			// 标题栏底边在宿主坐标系里的 y（挂在布局里，不能直接读几何）
			const QPoint belowBar = bar->mapTo(host, QPoint(0, bar->height()));
			area.setTop(belowBar.y());
		}
		return area;
	}

	// 外部一律走 showOverlayWithPopup：单独调它会让遮罩先于弹窗上屏
	static void showOverlay(QWidget* host, const void* owner)
	{
		if (!owner)
			return;

		QWidget* scrim = ensureOverlayWidget(host);
		if (!scrim)
			return;

		OverlayState& state = overlayStates()[host];
		state.owners.insert(owner);
		if (scrim->isVisible())
			return;   // 已经有弹窗开着，不重复铺

		const QRect area = overlayRect(host);
		scrim->setGeometry(area);
		scrim->raise();
		scrim->show();

		host->repaint(area);
	}

	void showOverlayWithPopup(QWidget* host, const void* owner, QWidget* popup)
	{
		if (!host)
			return;

		// 居中必须在 show() 之前做，否则会看到弹窗跳一下
		if (popup)
			popup->move(host->geometry().center() - popup->rect().center());

		showOverlay(host, owner);

		// 与上面那次重绘同一个事件循环轮次，合成器才会放进同一帧
		if (popup) {
			popup->show();
			popup->raise();
		}
	}

	void hideOverlay(QWidget* host, const void* owner)
	{
		if (!host || !owner)
			return;

		auto it = overlayStates().find(host);
		if (it == overlayStates().end())
			return;

		it.value().owners.remove(owner);
		if (!it.value().owners.isEmpty())
			return;   // 还有别的弹窗开着，遮罩得继续留着

		QWidget* scrim = it.value().widget;
		if (!scrim || !scrim->isVisible())
			return;

		const QRect area = overlayRect(host);
		scrim->hide();

		// 与紧接着的"隐藏弹窗"落在同一帧，所以调用方必须先收遮罩、再隐藏弹窗
		host->repaint(area);
	}

	void syncOverlay(QWidget* host)
	{
		if (!host)
			return;

		auto it = overlayStates().find(host);
		if (it == overlayStates().end() || !it.value().widget)
			return;

		QWidget* scrim = it.value().widget;
		if (!scrim->isVisible())
			return;   // 没显示就不用跟着 resize

		scrim->setGeometry(overlayRect(host));
	}

	void minimize(QWidget* window)
	{
		if (window)
			window->showMinimized();
	}

	void toggleMaximize(QWidget* window)
	{
		if (!window)
			return;
		if (window->isMaximized())
			window->showNormal();
		else
			window->showMaximized();
	}

	void closeWindow(QWidget* window)
	{
		if (window)
			window->close();
	}
}
