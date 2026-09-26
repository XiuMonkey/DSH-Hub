#include "ExtensionSystem/UiStage.h"

#include <QApplication>
#include <QDebug>
#include <QHash>
#include <QMainWindow>
#include <QThread>
#include <QWidget>

namespace
{
	// 宿主窗口的架空状态；用哈希表而非单例：切主题时新旧窗口会短暂共存
	struct StageState
	{
		QString owner;
		QWidget* nativeCentral = nullptr;
		QWidget* stage = nullptr;
		int captionTop = 0;
		int captionHeight = 0;
	};

	// 窗口销毁时靠 destroyed 摘掉自己的条目，否则表会随切主题长大
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

	// 宿主浮层是独立顶层窗口，不跟着客户区下线；必须用 close() 才走到 closeEvent →
	// WindowFrame::hideOverlay 释放遮罩，只 hide() 会把遮罩留在原地盖住舞台
	void closeHostPopups(QMainWindow* host)
	{
		const QWidgetList tops = QApplication::topLevelWidgets();
		for (QWidget* w : tops) {
			if (!w || w == host || !w->isWindow())
				continue;
			if (w->parentWidget() != host)
				continue;
			if (!w->isVisible())
				continue;
			qInfo("[UiStage] 关掉宿主浮层: %s", qPrintable(w->objectName()));
			w->close();
		}

		if (QWidget* scrim = host->findChild<QWidget*>(QStringLiteral("windowScrim"))) {
			if (scrim->isVisible())
				scrim->hide();
		}
	}

	void raiseInitOverlay(QMainWindow* host)
	{
		// 抢台在 buildUi() 的 raise() 之后，新客户区会盖住"初始化中"遮罩
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
				return st.stage;  // 幂等：同一个 owner 再调一次（切主题后的重挂）
			qWarning("[UiStage] %s 想抢台，但已被 %s 占着，拒绝",
				qPrintable(owner), qPrintable(st.owner));
			return nullptr;
		}

		static const char* const kHooked = "uiStageDestroyHooked";
		if (!host->property(kHooked).toBool()) {
			host->setProperty(kHooked, true);
			QObject::connect(host, &QObject::destroyed, [host]() { states().remove(host); });
		}

		closeHostPopups(host);

		// 原生客户区必须挂回窗口当普通子控件再 hide()：无父顶层窗口会丢 QSS 继承并混进 topLevelWidgets()
		st.nativeCentral = host->takeCentralWidget();
		if (st.nativeCentral) {
			st.nativeCentral->setParent(host);
			st.nativeCentral->hide();
		}

		// 舞台常驻不销毁：避免延迟销毁落到插件 unload() 之后（那时 vtable 已解映射）
		if (!st.stage) {
			st.stage = new QWidget(host);
			st.stage->setObjectName(QStringLiteral("uiStage"));
			// 刻意不叫 dshhubCentral、也不设 WA_StyledBackground：宿主 QSS 挂在 #dshhubCentral 后代选择器上
		}
		// 顺序要紧：先 setCentralWidget 再 show()，反过来会短暂变成顶层窗口
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

		// 舞台摘下来但不销毁（常驻复用）；里面控件由扩展自己删
		QWidget* stage = host->takeCentralWidget();
		if (stage)
			stage->hide();

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
