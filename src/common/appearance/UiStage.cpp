#include "common/appearance/UiStage.h"

#include <QApplication>
#include <QDebug>
#include <QHash>
#include <QMainWindow>
#include <QThread>
#include <QWidget>

namespace
{
	// 一个宿主窗口的架空状态。
	// 用哈希表而不是单例字段：正常情况下只有一个主窗口，但切主题时
	// "新窗口先建、旧窗口下一轮事件循环才析构"，两者会短暂共存。
	struct StageState
	{
		QString owner;                 // 空 = 没人在架空
		QWidget* nativeCentral = nullptr;  // takeCentralWidget() 摘下来的原生客户区
		QWidget* stage = nullptr;          // 交给扩展的舞台（常驻，只切可见面）
		int captionTop = 0;
		int captionHeight = 0;
	};

	// 只按宿主窗口指针记。窗口销毁时靠 destroyed 连接把自己的条目摘掉
	// （否则这张表会随切主题逐次长大）。
	QHash<const QWidget*, StageState>& states()
	{
		static QHash<const QWidget*, StageState> s_states;
		return s_states;
	}

	bool onGuiThread()
	{
		const QCoreApplication* app = QCoreApplication::instance();
		return app && QThread::currentThread() == app->thread();
	}

	void warnOffThreadOnce(const char* entry)
	{
		static bool warned = false;
		if (warned)
			return;
		warned = true;
		qWarning("[UiStage] %s 在非 GUI 线程被调用，已拒绝。"
			"本模块的状态与它操作的都是 UI 对象，请自己把调用投到 GUI 线程"
			"（见 ToolRequestDispatcher.h）。", entry);
	}

	// 宿主自己的浮层（设置 / 插件市场 / 扩展管理 / 工具过滤 / 模型选择菜单…）
	// 是**独立顶层窗口**，不会跟着客户区一起下线 —— 不收掉就会直接飘在扩展画面上。
	//
	// 这里刻意用 close() 而不是 hide()：close() 会走到各自的 closeEvent →
	// PopupWindow::closed() → 对应窗口的 WindowFrame::hideOverlay，遮罩那一层
	// 才会被正确 release（WindowFrame 的遮罩按 owner 记名，只有对应调用方
	// release 才真正隐藏）。只 hide() 会把遮罩留在原地，直接盖住舞台。
	void closeHostPopups(QMainWindow* host)
	{
		const QWidgetList tops = QApplication::topLevelWidgets();
		for (QWidget* w : tops) {
			if (!w || w == host || !w->isWindow())
				continue;
			// 只碰"挂在宿主名下的"顶层窗口，别人的窗口一概不动
			// （Tooltip 那个常驻单例是无父的，不在其列；它靠鼠标移开自己收）
			if (w->parentWidget() != host)
				continue;
			if (!w->isVisible())
				continue;
			qInfo("[UiStage] 关掉宿主浮层: %s", qPrintable(w->objectName()));
			w->close();
		}

		// 兜底：万一还有遮罩挂着（例如关闭被某处拦下），显式收一次。
		// 宁可少一层遮罩，也不能让它盖在舞台上面。
		if (QWidget* scrim = host->findChild<QWidget*>(QStringLiteral("windowScrim"))) {
			if (scrim->isVisible())
				scrim->hide();
		}
	}

	void raiseInitOverlay(QMainWindow* host)
	{
		// 初始化遮罩是 buildUi() 里 raise() 起来的，而抢台发生在它之后
		// （ClientExtension::loadAll() 在 DSHHub 构造末尾），新挂进去的客户区
		// 会盖住它 —— 把遮罩重新顶上来，"初始化中"那张卡才看得见。
		if (QWidget* init = host->findChild<QWidget*>(QStringLiteral("initOverlay")))
			init->raise();
	}
}

namespace UiStage
{
	QWidget* acquire(QMainWindow* host, const QString& owner)
	{
		if (!host || owner.isEmpty())
			return nullptr;
		if (!onGuiThread()) {
			warnOffThreadOnce("UiStage::acquire");
			return nullptr;
		}

		StageState& st = states()[host];
		if (st.stage) {
			if (st.owner == owner)
				return st.stage;   // 幂等：同一个 owner 再调一次（切主题后的重挂）
			qWarning("[UiStage] %s 想抢台，但已被 %s 占着，拒绝",
				qPrintable(owner), qPrintable(st.owner));
			return nullptr;
		}

		// 一次性接线：宿主窗口销毁时把自己的条目摘掉
		static const char* const kHooked = "uiStageDestroyHooked";
		if (!host->property(kHooked).toBool()) {
			host->setProperty(kHooked, true);
			QObject::connect(host, &QObject::destroyed, [host]() { states().remove(host); });
		}

		// 1) 宿主自己的浮层先收摊（否则会飘在扩展界面上）
		closeHostPopups(host);

		// 2) 原生客户区下线：**重新挂回窗口当普通子控件再 hide()**。
		//    不要让它变成无父顶层窗口 —— 那样它会丢掉窗口样式表的继承
		//    （QSS 是按顶层窗口挂的，见 ThemeManager::applyToWindow），还会混进
		//    QApplication::topLevelWidgets()，而 ThemeManager::reload() 正是按
		//    那张表刷样式的。
		st.nativeCentral = host->takeCentralWidget();
		if (st.nativeCentral) {
			st.nativeCentral->setParent(host);
			st.nativeCentral->hide();
		}

		// 3) 舞台常驻、只切可见面。还台时不销毁它 —— 避免"延迟销毁落到插件
		//    unload() 之后"（那时插件的 vtable 已解映射，析构一次就是崩）。
		if (!st.stage) {
			st.stage = new QWidget(host);
			st.stage->setObjectName(QStringLiteral("uiStage"));
			// 刻意**不叫** dshhubCentral、也不设 WA_StyledBackground：
			// 宿主的 QSS 全是 "#dshhubCentral <后代>" 的选择器（例如
			// defaults.qss 的通用 QPushButton / QLineEdit 规则），舞台作为
			// 原生客户区的**兄弟**才能天然免疫这套级联 —— 这也是"完全自绘"
			// 不需要扩展去对抗宿主样式表的原因。
		}
		// 顺序要紧：先 setCentralWidget（会 reparent 进窗口），再 show()。
		// 反过来在没有父的前提下 show()，会让它短暂变成一个顶层窗口。
		host->setCentralWidget(st.stage);
		st.stage->show();
		st.owner = owner;

		raiseInitOverlay(host);

		qInfo("[UiStage] %s 已架空宿主客户区（原生控件树已下线但保留）", qPrintable(owner));
		return st.stage;
	}

	bool release(QMainWindow* host, const QString& owner)
	{
		if (!host)
			return false;
		if (!onGuiThread()) {
			warnOffThreadOnce("UiStage::release");
			return false;
		}

		const auto it = states().find(host);
		if (it == states().end() || !it->stage)
			return false;
		if (!owner.isEmpty() && it->owner != owner) {
			qWarning("[UiStage] %s 想还台，但当前 owner 是 %s，拒绝",
				qPrintable(owner), qPrintable(it->owner));
			return false;
		}

		StageState& st = it.value();
		const QString who = st.owner;

		// 舞台摘下来但不销毁（常驻复用）。里面扩展的控件由扩展自己删 —— 见头文件说明。
		QWidget* stage = host->takeCentralWidget();   // == st.stage
		if (stage)
			stage->hide();

		// 原生界面原样回来：消息、滚动位置、输入内容都还在，不需要任何重载
		if (st.nativeCentral) {
			host->setCentralWidget(st.nativeCentral);
			st.nativeCentral->show();
			st.nativeCentral = nullptr;
		}
		st.owner.clear();
		st.captionTop = 0;
		st.captionHeight = 0;

		raiseInitOverlay(host);

		qInfo("[UiStage] %s 已还台（原生界面恢复）", qPrintable(who));
		return true;
	}

	int releaseForOwner(const QString& owner)
	{
		if (owner.isEmpty() || !onGuiThread())
			return 0;

		int n = 0;
		const QWidgetList tops = QApplication::topLevelWidgets();
		for (QWidget* w : tops) {
			auto* win = qobject_cast<QMainWindow*>(w);
			if (!win)
				continue;
			if (ownerOf(win) != owner)
				continue;
			if (release(win, owner))
				++n;
		}
		return n;
	}

	QString ownerOf(const QWidget* host)
	{
		if (!host)
			return QString();
		const auto it = states().constFind(host);
		return it == states().constEnd() ? QString() : it->owner;
	}

	bool isTakenOver(const QWidget* host)
	{
		if (!host)
			return false;
		const auto it = states().constFind(host);
		return it != states().constEnd() && it->stage != nullptr;
	}

	void setCaptionBand(QWidget* host, int top, int height)
	{
		if (!host)
			return;
		const auto it = states().find(host);
		if (it == states().end())
			return;
		it->captionTop = qMax(0, top);
		it->captionHeight = qMax(0, height);
	}

	bool captionBand(const QWidget* host, int* top, int* height)
	{
		if (!host)
			return false;
		const auto it = states().constFind(host);
		if (it == states().constEnd() || it->captionHeight <= 0)
			return false;
		if (top)
			*top = it->captionTop;
		if (height)
			*height = it->captionHeight;
		return true;
	}
}
