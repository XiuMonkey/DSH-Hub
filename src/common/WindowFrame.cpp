// ------------------------------------------------------------------
// WindowFrame.cpp
// ------------------------------------------------------------------
// 无边框窗口的窗口级功能逻辑实现（见 WindowFrame.h 的分工说明）。
// 这里只有判定与平台调用，没有一行绘制代码。
// ------------------------------------------------------------------

#include "WindowFrame.h"

#include <QByteArray>
#include <QLayout>
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

	QWidget* titleBarOf(QWidget* host)
	{
		return host ? host->findChild<QWidget*>(QLatin1String(kTitleBarObjectName)) : nullptr;
	}

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
#else
		Q_UNUSED(window);
		Q_UNUSED(titleBar);
		Q_UNUSED(pos);
#endif
		return HTCLIENT;
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

	QRect overlayRect(const QWidget* host)
	{
		if (!host)
			return QRect();

		QRect area = host->rect();
		const QWidget* bar = host->findChild<QWidget*>(QLatin1String(kTitleBarObjectName));
		if (bar) {
			// 标题栏底边在宿主坐标系里的 y（标题栏挂在中央容器的布局里，不能直接读几何）
			const QPoint belowBar = bar->mapTo(host, QPoint(0, bar->height()));
			area.setTop(belowBar.y());
		}
		return area;
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
