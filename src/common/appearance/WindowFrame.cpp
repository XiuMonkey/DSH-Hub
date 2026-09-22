// ------------------------------------------------------------------
// WindowFrame.cpp
// ------------------------------------------------------------------
// 无边框窗口的窗口级功能逻辑实现（见 WindowFrame.h 的分工说明）。
// 这里只有判定与平台调用，没有一行绘制代码。
// ------------------------------------------------------------------

#include "common/appearance/WindowFrame.h"

// 架空（VirtualShell）：本文件只在两处问它 —— 遮罩覆盖范围、窗口条命中测试。
// 这也是 common/appearance → ExtensionSystem 的唯一一条依赖边。
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

// 老 SDK 的 dwmapi.h 里没有这两个常量（Windows 11 新增）
#  ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#    define DWMWA_WINDOW_CORNER_PREFERENCE 33
#  endif
#  ifndef DWMWCP_DONOTROUND
#    define DWMWCP_DONOTROUND 1
#  endif
#endif

namespace
{
	// 边缘缩放热区宽度（逻辑像素）
	constexpr int kResizeBorder = 5;

	// --- 控件侧的约定名（见 WindowFrame.h）---
	const char kTitleBarObjectName[] = "windowTitleBar";
	const char kWindowControlProperty[] = "dshWindowControl";
	const char kMaximizedProperty[] = "maximized";

	// 命中测试用：该点是否落在标题栏的窗口按钮上
	bool isWindowControlAt(QWidget* window, const QPoint& clientPos)
	{
		for (QWidget* w = window->childAt(clientPos); w && w != window; w = w->parentWidget()) {
			if (w->property(kWindowControlProperty).toBool())
				return true;
		}
		return false;
	}

	// 把 WM_NCHITTEST 的返回值算出来（pos 是**逻辑**客户区坐标）
	qintptr hitTest(QWidget* window, const QWidget* titleBar, const QPoint& pos)
	{
#ifdef Q_OS_WIN
		// 1) 四周边缘 = 缩放热区（最大化/全屏时不可缩放）
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

		// 2) 自绘标题栏交给系统当标题栏：拖动、双击最大化、贴边吸附、右键系统菜单
		//    全部由系统完成，Qt 侧一行拖动代码都不用写。
		//    按钮区域必须排除，否则按钮永远收不到鼠标事件。
		if (titleBar && titleBar->isVisible()) {
			const QRect titleRect(titleBar->mapTo(window, QPoint(0, 0)), titleBar->size());
			if (titleRect.contains(pos) && !isWindowControlAt(window, pos))
				return HTCAPTION;
		}

		// 2b) 架空（UiStage）时原生标题栏被藏起来了，回落到**扩展登记的自绘窗口条**。
		//     没有这条回落，扩展自绘的界面就拖不动、双击不最大化、贴边不吸附 ——
		//     它只能靠 Alt+Space 挪窗口，等于窗口级交互全丢。
		//     isWindowControlAt 那套 dshWindowControl 动态属性约定照旧生效，
		//     所以扩展自己的最小化 / 关闭按钮仍然点得到。
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

	// --- 半透明遮罩的登记表（见 WindowFrame.h 的 showOverlay/hideOverlay）---
	// 每个宿主窗口一条：遮罩控件 + 当前在用它的调用方。
	// 宿主窗口正常情况下只有一个，所以这份表实际只有一项。
	const char kScrimObjectName[] = "windowScrim";

	struct OverlayState
	{
		// 遮罩是宿主的子控件，宿主析构时由 Qt 一起删掉；这里只留弱引用
		QPointer<QWidget> widget;
		QSet<const void*> owners;   // 谁在用（按调用方 this 记名）
	};

	QHash<const QWidget*, OverlayState>& overlayStates()
	{
		static QHash<const QWidget*, OverlayState> states;
		return states;
	}

	// 取（必要时创建）宿主的遮罩控件；host 为空时返回 nullptr
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

		// 宿主销毁时把登记项收掉（遮罩控件本身随宿主一起销毁，这里不要碰它）。
		// 连接挂在 sender 上，宿主没了连接自然断掉。
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
		// 幂等：样式位已经补过就不再折腾（重复 SetWindowPos 会打断最大化动画）
		if ((style & (WS_THICKFRAME | WS_CAPTION)) == (WS_THICKFRAME | WS_CAPTION))
			return;

		// Qt::FramelessWindowHint 会让 Qt 摘掉 WS_CAPTION / WS_THICKFRAME。这里补回来：
		// 窗口在系统眼里依旧“有边框”，于是系统投影、贴边吸附（Aero Snap）、最大化
		// 贴合工作区、右键系统菜单等原生行为全部保留；而边框实际占的位置由
		// WM_NCCALCSIZE 归零（见 handleNativeMessage），用户看到的只有自绘的圆角描边。
		style |= WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
		SetWindowLongPtrW(hwnd, GWL_STYLE, style);

		// Windows 11 默认给窗口加 8px 系统圆角，与自绘的 12px 不一致，会裁掉描边的
		// 四个角；关掉系统圆角，只留投影。Windows 10 上没有这个属性，调用失败即可。
		const BOOL doNotRound = DWMWCP_DONOTROUND;
		DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &doNotRound, sizeof(doNotRound));

		// 让系统按新样式重算外框（与 WM_NCCALCSIZE 的“客户区铺满窗口”配合）
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
				// 返回 0：客户区 = 整个窗口，系统标题栏与边框不占任何位置。
				// 这个等式必须“永远”成立：最大化时若改成把客户区钉到工作区，
				// 客户区就比窗口矩形小一圈，而 Qt 是按“窗口矩形 + 自身缓存的
				// 外框边距(0)”换算窗口几何的 —— 两者一旦不一致，Qt 的重绘目标
				// 就落到可视区之外，窗口表面再也不更新（画面冻结、UI 位置和
				// 实际命中位置对不上）。系统最大化多出来的那一圈由
				// applyMaximizedContentInset() 在内容层补回来。
				*result = 0;
				return true;
			}
			break;
		case WM_NCHITTEST: {
			// WM_NCHITTEST 的坐标是“物理像素”，而 Qt 的控件几何是“逻辑像素”
			// （高 DPI 下两者差一个缩放比），必须换算后再做命中判定，
			// 否则整块窗口都会被当成缩放热区。
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

	// 系统最大化矩形 = 工作区 + 一圈不可见的缩放边框（200% 缩放下约 13px）。
	// 客户区被我们铺满整个窗口，那一圈就跑到屏幕外/任务栏后面去了，于是贴在窗口右缘的
	// 窗口按钮会被屏幕边缘裁掉一小截。
	// 客户区不能再动（见 WM_NCCALCSIZE），而带 WS_MAXIMIZE 的窗口又没法用 SetWindowPos
	// 挪动（系统会把它拉回最大化矩形），所以改成在内容层把这一圈补回来，
	// 让可见内容正好落进工作区。
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
					// 四边取最小值：正常情况下四边相等，异常时宁可少补也不要多补
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

	// 边框收尾的组合动作：判定是否贴屏幕边缘，再分别落到"圆角描边状态"与
	// "最大化内容补偿"两步上。返回 edgeToEdge，供调用方刷新标题栏图标。
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

		// 架空（UiStage）时"标题栏以下"这个概念不成立：整块客户区都是扩展的画布，
		// 所以遮罩要铺满。不回退的话，扩展弹出自己的窗口时遮罩会短一截，
		// 顶部留出一条没被压暗的缝（那一截本来是为宿主自绘标题栏让出来的）。
		if (UiStage::isTakenOver(host))
			return area;

		const QWidget* bar = host->findChild<QWidget*>(QLatin1String(kTitleBarObjectName));
		if (bar) {
			// 标题栏底边在宿主坐标系里的 y（标题栏挂在中央容器的布局里，不能直接读几何）
			const QPoint belowBar = bar->mapTo(host, QPoint(0, bar->height()));
			area.setTop(belowBar.y());
		}
		return area;
	}

	// 文件私有：外部一律走 showOverlayWithPopup —— 单独调它会让遮罩先于弹窗上屏。
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
			return;   // 已经有弹窗开着，这一层就是那层，不重复铺

		const QRect area = overlayRect(host);
		scrim->setGeometry(area);
		scrim->raise();
		scrim->show();

		// 同步重绘：让遮罩立刻落到宿主表面上（理由见 showOverlayWithPopup）
		host->repaint(area);
	}

	void showOverlayWithPopup(QWidget* host, const void* owner, QWidget* popup)
	{
		if (!host)
			return;

		// 居中放在这里做：move() 要用弹窗的 rect()，而且必须在 show() 之前 ——
		// show 之后再挪会看到弹窗跳一下。
		if (popup)
			popup->move(host->geometry().center() - popup->rect().center());

		showOverlay(host, owner); // 末尾带一次宿主同步重绘

		// 紧接着映射弹窗：与上面那次重绘同一个事件循环轮次，合成器才会放进同一帧
		if (popup) {
			popup->show();
			popup->raise();
		}
	}

	void hideOverlay(QWidget* host, const void* owner)
	{
		if (!host || !owner)
			return;

		// 只认领过 showOverlay 的宿主：没登记过就没有遮罩要收
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

		// 同步重绘一次：把遮罩立刻从屏幕上擦掉，和紧接着的"隐藏弹窗"落在同一帧
		// （理由见头文件）。所以调用方必须先收遮罩、再隐藏弹窗。
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