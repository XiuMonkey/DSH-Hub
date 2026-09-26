#include "ExtensionSystem/ClientExtension.h"
#include "core/DshHostPlugin.h"
#include "core/HostExports.h"
#include "common/util/CommonRegistry.h"
#include "VirtualClass/VirtualApiTakeover.h"
#include "ExtensionSystem/UiStage.h"

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
		// 只在 GUI 线程读写，不加锁
		QStringList g_loadedNames;

		// 已装载扩展的 loader（键 = 扩展名，所有权在 qApp）
		QHash<QString, QPluginLoader*> g_loaders;

		// 卸载时删不掉的目录留下这个标记，下次启动由 sweepPendingRemovals() 清掉
		const char* const kPendingRemovalMarker = ".pending-removal";

		// 插件可选槽"我要退场了"：元对象层字符串契约（非接口方法），有它就说明 invoke 之后
		// 宿主再也不会用到插件的任何对象；老插件没有它也不会踩空 vtable 槽位
		const char* const kDetachSlot = "detachHost()";

		// 可选槽：宿主在 attachHost() 之前推一次本扩展的**安装目录名**。架空舞台要求 owner 等于
		// 安装名才能做所有权校验、并在卸载时把这个扩展占的台强制收回，插件于是不必硬编码自己的名字
		const char* const kIdentitySlot = "setHostIdentity(QString)";

		bool onGuiThread()
		{
			const QCoreApplication* app = QCoreApplication::instance();
			return app && QThread::currentThread() == app->thread();
		}

		// regulation.json5 是"已安装"的判据
		QString regulationPath(const QString& dir)
		{
			return dir + QStringLiteral("/regulation.json5");
		}

		// 只认两种形状：带我们的标记文件，或没有 regulation.json5 但里面还有 dll
		bool looksLikeRemovalLeftover(const QDir& dir)
		{
			if (QFile::exists(dir.filePath(QLatin1String(kPendingRemovalMarker))))
				return true;
			if (QFile::exists(dir.filePath(QStringLiteral("regulation.json5"))))
				return false;
			return !dir.entryList({ QStringLiteral("*.dll") }, QDir::Files).isEmpty();
		}

		// 只在启动期（loadAll 之前）调用：那时 dll 不再被映射，删除必然成功
		void sweepPendingRemovals()
		{
			QDir root(extensionDirectory());
			if (!root.exists())
				return;

			const QFileInfoList subDirs = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
			for (const QFileInfo& sub : subDirs) {
				const QDir dir(sub.absoluteFilePath());
				if (!looksLikeRemovalLeftover(dir))
					continue;

				const QString path = sub.absoluteFilePath();
				if (QDir(path).removeRecursively())
					qInfo("[ClientExtension] swept removal leftover: %s", qPrintable(sub.fileName()));
				else
					qWarning("[ClientExtension] cannot sweep leftover (still locked?): %s", qPrintable(path));
			}
		}

		bool loadOneImpl(const QString& dllPath, const QString& rawName, QString* error)
		{
			const QString name = rawName.isEmpty() ? QFileInfo(dllPath).completeBaseName() : rawName;
			const QString fileName = QFileInfo(dllPath).fileName();

			if (g_loadedNames.contains(name)) {
				// 已装载：不重复装载 dll，但要重新 attach —— 切主题会重建主窗口
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

			// 顺序别改回一句 QPluginLoader(dllPath)：构造函数会带上 PreventUnloadHint，而
			// setLoadHints() 只在尚未关联文件时才生效，故必须先构造空的、清 hint、再 setFileName
			auto* loader = new QPluginLoader(QCoreApplication::instance());
			loader->setLoadHints(QLibrary::LoadHints());
			loader->setFileName(dllPath);

			QObject* root = loader->instance();
			if (!root) {
				// Qt 会在这里拦掉不兼容的插件（版本不符、debug/release 混用），做法补在原因后面
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

			// 两个成功分支都登记：装载失败的那个同样会让目录删不掉
			g_loaders.insert(name, loader);

			auto* plugin = qobject_cast<DshHostPlugin*>(root);
			if (!plugin) {
				const QString reason = QStringLiteral("does not implement DshHostPlugin");
				if (error)
					*error = reason;
				qWarning("[ClientExtension] %s: %s", qPrintable(fileName), qPrintable(reason));
				return false; // loader 留在 qApp 下，随进程一起走
			}

			// 可选能力：只登记、不装载判定，拒绝发生在 Takenover(true) 时。插件侧必须在根对象
			// 上写 `Q_INTERFACES(DshHostPlugin VirtualApiSink)`，否则 cast 永远是 nullptr
			if (qobject_cast<VirtualApiSink*>(root)) {
				CommonRegistry::instance().AddToRegistry(DshHostIndex::kApiSink, root);
				qInfo("[ClientExtension] %s: 后端接管接收端（VirtualApiSink）已登记", qPrintable(name));
			}
			else {
				qInfo("[ClientExtension] %s: 未实现 VirtualApiSink —— 它不能接管后端出站", qPrintable(name));
			}

			// 可选槽：插件要架空就必须知道自己的安装名（老插件没有也不踩空槽位）
			if (root->metaObject()->indexOfMethod(kIdentitySlot) >= 0) {
				QMetaObject::invokeMethod(root, "setHostIdentity", Qt::DirectConnection, Q_ARG(QString, name));
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

		// 卸载成功 ⇒ 模块解映射 ⇒ 文件锁松开 ⇒ 目录马上能删干净。前提是装载时 PreventUnloadHint
		// 已清掉、且插件声明可安全退场；detachHost() 里必须删掉自己挂在宿主控件树里的东西
		if (QPluginLoader* loader = g_loaders.take(name)) {
			QObject* root = loader->instance();
			const bool canDetach = root && root->metaObject()->indexOfMethod(kDetachSlot) >= 0;
			if (canDetach) {
				QMetaObject::invokeMethod(root, "detachHost", Qt::DirectConnection);

				// unload() 自己会把根组件删掉，这里不该手工 delete 插件对象
				const bool unloaded = loader->unload();
				qInfo("[ClientExtension] remove: detach+unload %s -> %s", qPrintable(name),
					unloaded ? "ok" : "failed (loader refused)");

				// 卸载成功必须把"已装载"记录一起摘掉：实例已销毁、dll 也解映射，名字不再代表活着
				// 的东西；否则"移除 → 再安装"会走"重新 attach"分支却找不到 loader，扩展按钮不出来
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

		// 宿主**强制收回架空**（幂等），不能指望插件自觉。位置刻意在 unload() **之后**：release()
		// 只把舞台 hide() 掉，不碰扩展留在舞台里的控件（那些 vtable 可能已经解映射）
		if (const int stages = UiStage::releaseForOwner(name); stages > 0)
			qInfo("[ClientExtension] %s: 宿主强制收回架空（%d 个窗口）", qPrintable(name), stages);

		// 删判据文件即"已卸载"：不再出现在列表，下次启动也不会被装载
		const QString regulation = regulationPath(dir);
		if (QFile::exists(regulation) && !QFile::remove(regulation)) {
			if (error)
				*error = QStringLiteral("cannot remove %1").arg(regulation);
			return false;
		}

		if (QDir(dir).removeRecursively()) {
			qInfo("[ClientExtension] removed: %s (dir deleted)", qPrintable(name));
			return true;
		}

		// 还有文件删不掉（被本进程映射着、插件没声明可退场）：留标记，下次启动收尾。
		// 注意 Windows 上"改名/移动目录"也躲不开这个锁，所以不做那种花招
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

		// 清掉上次没删干净的残留：那时 dll 还锁着，新进程里不再锁
		sweepPendingRemovals();

		const QDir dir(extensionDirectory());
		if (!dir.exists())
			return loaded; // 没装客户端扩展是常态，不报错也不建目录

		const QFileInfoList subDirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
		for (const QFileInfo& sub : subDirs) {
			const QDir subDir(sub.absoluteFilePath());

			// 没有 regulation.json5 的一律跳过：那是移除时 DLL 被锁留下的残留
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
