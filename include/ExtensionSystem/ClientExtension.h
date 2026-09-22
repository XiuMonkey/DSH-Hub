#pragma once

// ------------------------------------------------------------------
// ClientExtension.h
// ------------------------------------------------------------------
// 客户端扩展：装进本进程、在 GUI 线程直接操作宿主界面的 QPlugin DLL。
//
// ⚠️ 与「工具扩展」（DllCaller 那条线）**不是一套东西**，只是都用 .ext 交付：
//   工具扩展   → 装进服务端扩展目录，Worker 线程跑 JSON 工具调用，登记在 extensions.json；
//   客户端扩展 → 装进 <exe>/clientExtensions/，GUI 线程用 QPluginLoader 装载，
//                登记在本文件自己的列表里。
//   分流在 ExtensionLoader::loadAndInstall 里按 regulation 的 Type 字段做。
//
// 装载与 attach **必须**在 GUI 线程：插件在 attachHost() 里要查注册表、
// 往宿主控件树挂控件（见 core/DshHostPlugin.h）。
// ------------------------------------------------------------------

#include <QString>
#include <QStringList>

namespace ClientExtension
{
	// 目录：默认 <exe>/clientExtensions，环境变量 DSHHUB_CLIENT_EXTENSION_DIR 可覆盖。
	QString extensionDirectory();

	// 已装载的扩展名（取自 regulation 的 Name）。同时是防重复装载的依据。
	QStringList loadedNames();

	// 已安装的扩展名：扫 <dir>/<Name>/regulation.json5 —— 安装与移除都以它为判据。
	// 与 loadedNames() 的区别：这个反映**磁盘状态**（重启后仍在、装载失败也在）。
	QStringList installedNames();

	// 移除一个已安装的扩展。两条路径：
	//   1) 插件声明了 detachHost() 槽（见 core/DshHostPlugin.h 的说明）→ 先让它把自己
	//      挂在宿主里的东西拆掉，再 QPluginLoader::unload()。unload() 会顺手删掉插件的
	//      根组件，dll 随之解映射 ⇒ **目录当场删干净**（前提是装载时清掉了
	//      PreventUnloadHint，见 .cpp 里 loadOneImpl 那段）。
	//   2) 没声明（老插件）→ 删 regulation.json5（判据，必定成功），再尽力删目录：被
	//      本进程映射着的 dll 删不掉，于是写 .pending-removal 标记，由**下次启动**的
	//      loadAll() 清扫收尾。
	//
	// 返回值 = "目录是否真的删干净了"：
	//   true  → 目录没了；
	//   false → 判据已删（不再算已安装、下次启动也不会装载），但还有文件被占用删不掉，
	//           error 里会带回一句可直接展示的说明。
	//
	// 顺带说明：Windows 上改名/移动目录也躲不开这个锁（实测：目录里有被占用的文件时，
	// 连父目录改名都会被拒），所以这里不做那种花招。
	bool remove(const QString& name, QString* error = nullptr);

	// regulation 的 Type 是否为客户端扩展类型（ClientExtension[Debug]，大小写不敏感）。
	bool isClientExtensionType(const QString& type);

	// 扫目录并逐个装载（取 <dir>/<Name>/ 下的 dll，main.dll 优先）。
	// 必须在 GUI 线程、且窗口与注册表就绪之后调用。返回本次成功装载的名字。
	QStringList loadAll();

	// 装载指定的一个 DLL。名字已在 loadedNames() 里则跳过（不重复 attach）。
	// 必须在 GUI 线程调用。失败原因写进 error（可选）。
	bool loadOne(const QString& dllPath, const QString& name, QString* error = nullptr);
}
