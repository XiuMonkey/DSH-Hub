#include "ExtensionSystem/ClientExtension.h"

#include "core/DshHostPlugin.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLibrary>
#include <QPluginLoader>
#include <QThread>
#include <QtGlobal>

namespace ClientExtension
{
	namespace
	{
		// 只在 GUI 线程读写（装载入口有线程断言），所以不加锁。
		QStringList g_loadedNames;

		// 已装载扩展的 QPluginLoader：移除时拿它 unload。
		//
		// unload() 成不成，取决于装载时那个 PreventUnloadHint 有没有清掉（见 loadOneImpl
		// 里那段说明）。清掉了 ⇒ unload() 真的解映射，文件锁当场松开、目录能马上删干净；
		// 没清掉 ⇒ unload() 会返回 true 但模块仍被映射，只能走"标记 + 下次启动清扫"。
		// 键 = 扩展名（与 g_loadedNames 同源）。
		//
		// 所有权仍在 qApp 名下（见 loadOneImpl）：这里只存裸指针。
		QHash<QString, QPluginLoader*> g_loaders;

		// 卸载时删不掉的文件所在目录会留下这个标记；下次启动（那时 dll 已不被占用）
		// 由 sweepPendingRemovals() 彻底清掉。
		const char* const kPendingRemovalMarker = ".pending-removal";

		// 插件可选实现的"我要退场了"槽（**元对象层的字符串契约，不是接口方法** ——
		// 所以老插件没有它也不会踩空 vtable 槽位，宿主用 indexOfMethod 一查就知道）。
		// 有它就说明作者保证：invoke 之后，宿主再也不会用到插件的任何对象。
		const char* const kDetachSlot = "detachHost()";

		bool onGuiThread()
		{
			const QCoreApplication* app = QCoreApplication::instance();
			return app && QThread::currentThread() == app->thread();
		}

		// regulation.json5 是"已安装"的判据：installedNames / loadAll / remove 三处共用
		QString regulationPath(const QString& dir)
		{
			return dir + QStringLiteral("/regulation.json5");
		}

		// 一个子目录是不是"卸载残留"？只认两种形状：
		//   · 带我们的标记文件（本轮卸载留下的）；或
		//   · 没有 regulation.json5（= 不再算已安装）但里面还有 dll（= 我们装进去的载荷）。
		// 别的一律不碰 —— 那不是我们建的目录，删错的代价比留个目录大。
		bool looksLikeRemovalLeftover(const QDir& dir)
		{
			if (QFile::exists(dir.filePath(QLatin1String(kPendingRemovalMarker))))
				return true;
			if (QFile::exists(dir.filePath(QStringLiteral("regulation.json5"))))
				return false;
			return !dir.entryList({ QStringLiteral("*.dll") }, QDir::Files).isEmpty();
		}

		// 清扫卸载残留。只在启动期（loadAll 之前）调用：那时上一个进程已经退出，
		// 插件 dll 不再被映射，删除必然成功。
		void sweepPendingRemovals()
		{
			QDir root(extensionDirectory());
			if (!root.exists())
				return;

			const QFileInfoList subDirs = root.entryInfoList(
				QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
			for (const QFileInfo& sub : subDirs) {
				const QDir dir(sub.absoluteFilePath());
				if (!looksLikeRemovalLeftover(dir))
					continue;

				const QString path = sub.absoluteFilePath();
				if (QDir(path).removeRecursively())
					qInfo("[ClientExtension] swept removal leftover: %s", qPrintable(sub.fileName()));
				else
					qWarning("[ClientExtension] cannot sweep leftover (still locked?): %s",
						qPrintable(path));
			}
		}

		bool loadOneImpl(const QString& dllPath, const QString& rawName, QString* error)
		{
			const QString name = rawName.isEmpty() ? QFileInfo(dllPath).completeBaseName() : rawName;
			const QString fileName = QFileInfo(dllPath).fileName();

			if (g_loadedNames.contains(name)) {
				// 已经装载过：**不重复装载 dll，但要重新 attach**。
				//
				// 为什么：切主题会重建主窗口（见 ThemeManager::switchTheme），插件上一次挂到
				// 顶栏/侧栏的控件随旧窗口一起没了；而 attachHost() 的设计本来就允许被调用
				// 多次（见 DshPlugin.h 里的说明），新窗口起来时正需要这一次"再挂一遍"。
				// 这里以前是直接 return false（等于什么都不做），表现就是"切完亮/暗主题，
				// 扩展的按钮消失了"。
				QPluginLoader* loader = g_loaders.value(name);
				QObject* root = loader ? loader->instance() : nullptr;
				auto* plugin = root ? qobject_cast<DshHostPlugin*>(root) : nullptr;
				if (plugin) {
					plugin->attachHost();
					qInfo("[ClientExtension] re-attached to the new window: %s", qPrintable(name));
					return true;
				}
				qWarning("[ClientExtension] %s: already loaded but cannot re-attach", qPrintable(name));
				return false;
			}

			// loader 以 qApp 为父对象：插件在整个进程存活期保持加载。刻意不用窗口当父对象 ——
			// 切主题会新建窗口、旧窗口稍后析构，而 qt_plugin_instance() 对同一个 DLL 返回
			// 同一个实例，跟着窗口走会试图卸载仍在使用的库。
			//
			// ⚠️ 装载顺序有讲究，别改回一句 QPluginLoader(dllPath)：
			//    QPluginLoader 的构造函数会顺手带上 QLibrary::PreventUnloadHint，而
			//    setLoadHints() **只有在尚未关联文件时才生效**（Qt 的约定）。所以必须先
			//    构造空的、清掉 hint、再 setFileName —— 这样移除时 unload() 才真能解映射，
			//    dll 文件锁当场松开（实测：不清则 unload() 返回 true 但文件仍被映射）。
			//    另一层坑：hint 挂在"每个 dll 路径一条"的库条目上，只要本进程里有**任何**
			//    一次 QPluginLoader(path) 先跑过，后面再按正确顺序也拿不到干净的条目。
			//    客户端扩展的装载只有这一条路径，所以这里清掉就是干净的。
			auto* loader = new QPluginLoader(QCoreApplication::instance());
			loader->setLoadHints(QLibrary::LoadHints());
			loader->setFileName(dllPath);

			QObject* root = loader->instance();
			if (!root) {
				// Qt 会在这里拦掉不兼容的插件（Qt 版本不符、debug/release 混用）。后者最常见
				// 且从错误文字看不出该怎么办，所以把做法补在后面。
				QString reason = loader->errorString();
				if (reason.contains(QStringLiteral("debug and release"), Qt::CaseInsensitive)) {
					reason += QStringLiteral(" —— 插件必须与宿主同一档编译"
						"（宿主是 Debug 就用 -DCMAKE_BUILD_TYPE=Debug 重编插件）");
				}
				if (error)
					*error = reason;
				qWarning("[ClientExtension] not a Qt plugin (or load failed): %s -- %s",
					qPrintable(fileName), qPrintable(reason));
				delete loader;
				return false;
			}

			// 登记 loader：移除时要拿它 unload。**两个成功分支都登记** —— 装载失败的
			// 那个（dll 被映射着、但没挂上宿主）同样会让目录删不掉。
			g_loaders.insert(name, loader);

			auto* plugin = qobject_cast<DshHostPlugin*>(root);
			if (!plugin) {
				const QString reason = QStringLiteral("does not implement DshHostPlugin");
				if (error)
					*error = reason;
				qWarning("[ClientExtension] %s: %s", qPrintable(fileName), qPrintable(reason));
				return false; // loader 留在 qApp 下，随进程一起走
			}

			plugin->attachHost();
			g_loadedNames.append(name);
			qInfo("[ClientExtension] loaded: name=%s dll=%s", qPrintable(name), qPrintable(fileName));
			return true;
		}
	}

	QString extensionDirectory()
	{
		const QString override = qEnvironmentVariable("DSHHUB_CLIENT_EXTENSION_DIR");
		if (!override.isEmpty())
			return override;

		return QCoreApplication::applicationDirPath() + QStringLiteral("/clientExtensions");
	}

	QStringList loadedNames()
	{
		return g_loadedNames;
	}

	QStringList installedNames()
	{
		QStringList names;

		const QDir dir(extensionDirectory());
		if (!dir.exists())
			return names;

		const QFileInfoList subDirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
		for (const QFileInfo& sub : subDirs) {
			if (QFile::exists(regulationPath(sub.absoluteFilePath())))
				names.append(sub.fileName());
		}
		return names;
	}

	bool remove(const QString& name, QString* error)
	{
		if (name.isEmpty()) {
			if (error)
				*error = QStringLiteral("empty extension name");
			return false;
		}

		const QString dir = extensionDirectory() + QStringLiteral("/") + name;
		if (!QDir(dir).exists()) {
			if (error)
				*error = QStringLiteral("not installed: %1").arg(name);
			return false;
		}

		// 1) 能当场卸载就当场卸载：卸载成功 ⇒ 模块解映射 ⇒ 文件锁松开 ⇒ 目录马上能删干净。
		//
		//    前提有两条：
		//      a) 装载时那个 PreventUnloadHint 已经清掉（见 loadOneImpl 的说明）；
		//      b) **插件声明自己可以安全退场** —— 宿主查它的元对象有没有 detachHost() 槽
		//         （元对象层的字符串契约，零 ABI 变更；老插件自然没有，就自动走下面的
		//         兜底路径，绝不会踩空 vtable 槽位）。
		//    插件在 detachHost() 里必须把自己挂在宿主控件树里的东西全删掉：那一刻之后
		//    宿主再也不会用到插件的任何对象 —— 卸载后那些对象的 vtable 指向已解映射的
		//    内存，碰一下就崩。
		if (QPluginLoader* loader = g_loaders.take(name)) {
			QObject* root = loader->instance();
			const bool canDetach = root && root->metaObject()->indexOfMethod(kDetachSlot) >= 0;
			if (canDetach) {
				QMetaObject::invokeMethod(root, "detachHost", Qt::DirectConnection);

				// QPluginLoader::unload() 自己会把根组件删掉（实测：返回 true 之后那个对象的
				// QPointer 立刻为空），所以这里不需要、也不该手工 delete 插件对象。
				const bool unloaded = loader->unload();
				qInfo("[ClientExtension] remove: detach+unload %s -> %s", qPrintable(name),
					unloaded ? "ok" : "failed (loader refused)");

				// ⚠️ 卸载成功就必须把"已装载"这条记录一起摘掉：插件实例已经被 unload() 销毁、
				//    dll 也解映射了，这个名字不再代表任何活着的东西。否则用户"移除 → 再安装"时，
				//    loadOneImpl 会因为名字还在表里而走"重新 attach"分支，但 loader 已经被上面
				//    take 掉了、实例也没了 ⇒ 只能报 "cannot re-attach" 并放弃装载 ——
				//    表现就是"重新装完，扩展按钮不出来"。
				if (unloaded)
					g_loadedNames.removeAll(name);

				if (unloaded && QDir(dir).removeRecursively()) {
					qInfo("[ClientExtension] removed: %s (unloaded + dir deleted)", qPrintable(name));
					return true;
				}
			}
			else if (root) {
				qInfo("[ClientExtension] %s has no %s slot: it cannot be unloaded safely,"
					" falling back to mark + sweep", qPrintable(name), kDetachSlot);
			}
		}

		// 2) 删判据文件（不是可执行文件、没被锁，必定成功）。删掉即"已卸载"：
		//    不再出现在已安装列表里，下次启动也不会被装载。
		const QString regulation = regulationPath(dir);
		if (QFile::exists(regulation) && !QFile::remove(regulation)) {
			if (error)
				*error = QStringLiteral("cannot remove %1").arg(regulation);
			return false;
		}

		// 3) 整目录删掉 —— 没被占用的扩展到这里就彻底干净了。
		if (QDir(dir).removeRecursively()) {
			qInfo("[ClientExtension] removed: %s (dir deleted)", qPrintable(name));
			return true;
		}

		// 4) 还有文件删不掉（典型就是被本进程映射着、且插件没声明可退场的 main.dll）。
		//    留一个标记，下次启动的 sweepPendingRemovals() 收尾 —— 那时 dll 不再被占用，
		//    必然删得掉。
		//
		//    注意：Windows 上"改名/移动目录"也躲不开这个锁（实测：目录里有被占用
		//    的文件时，连父目录改名都会被拒），所以这里不做那种花招。
		QFile marker(QStringLiteral("%1/%2").arg(dir, QLatin1String(kPendingRemovalMarker)));
		if (!marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
			qWarning("[ClientExtension] cannot write removal marker: %s", qPrintable(marker.fileName()));
		else
			marker.close();

		const QString leftovers = QDir(dir).entryList(QDir::Files, QDir::Name).join(QLatin1String(", "));
		qInfo("[ClientExtension] removed (files still locked): %s leftovers=[%s] dir=%s",
			qPrintable(name), qPrintable(leftovers), qPrintable(dir));
		if (error) {
			*error = QStringLiteral("%1 仍被本进程占用，当次删不掉（残留在 %2）；"
				"扩展已卸载，残留会在下次启动客户端时自动清掉。"
				"（插件若声明 %3 槽，移除时就能当场卸载并删干净）")
				.arg(leftovers.isEmpty() ? QStringLiteral("部分文件") : leftovers, dir,
					QLatin1String(kDetachSlot));
		}
		return false;
	}

	bool isClientExtensionType(const QString& type)
	{
		return type.compare(QStringLiteral("ClientExtension"), Qt::CaseInsensitive) == 0
			|| type.compare(QStringLiteral("ClientExtensionDebug"), Qt::CaseInsensitive) == 0;
	}

	QStringList loadAll()
	{
		QStringList loaded;

		if (!onGuiThread()) {
			qWarning("[ClientExtension] loadAll() must be called on the GUI thread, skipped");
			return loaded;
		}

		// 先把上次没删干净的残留清掉：那时 dll 还锁着，现在（新进程里）不再锁了。
		sweepPendingRemovals();

		const QDir dir(extensionDirectory());
		if (!dir.exists())
			return loaded; // 没装客户端扩展是常态，不报错也不建目录

		const QFileInfoList subDirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
		for (const QFileInfo& sub : subDirs) {
			const QDir subDir(sub.absoluteFilePath());

			// 没有 regulation.json5 的一律跳过：那是移除时 DLL 被锁、目录没删干净的残留。
			// 上面的 sweepPendingRemovals() 已经尽力删过一轮；走到这里说明这次也删不掉
			// （例如目录被别的程序占着），那就留着，下次再说。
			if (!QFile::exists(regulationPath(sub.absoluteFilePath()))) {
				qInfo("[ClientExtension] skip (no regulation.json5, likely a removal leftover): %s",
					qPrintable(sub.fileName()));
				continue;
			}

			QString dll = subDir.filePath(QStringLiteral("main.dll"));
			if (!QFile::exists(dll)) {
				const QStringList any = subDir.entryList({ QStringLiteral("*.dll") }, QDir::Files, QDir::Name);
				if (any.isEmpty())
					continue;
				dll = subDir.filePath(any.first());
			}
			if (loadOneImpl(dll, sub.fileName(), nullptr))
				loaded.append(sub.fileName());
		}

		return loaded;
	}

	bool loadOne(const QString& dllPath, const QString& name, QString* error)
	{
		if (!onGuiThread()) {
			const QString reason = QStringLiteral("loadOne() must be called on the GUI thread");
			if (error)
				*error = reason;
			qWarning("[ClientExtension] %s", qPrintable(reason));
			return false;
		}

		if (dllPath.isEmpty() || !QFile::exists(dllPath)) {
			const QString reason = QStringLiteral("dll not found: %1").arg(dllPath);
			if (error)
				*error = reason;
			qWarning("[ClientExtension] %s", qPrintable(reason));
			return false;
		}

		return loadOneImpl(dllPath, name, error);
	}
}