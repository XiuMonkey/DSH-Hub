#pragma once

// 客户端扩展：装进本进程、在 GUI 线程直接操作宿主界面的 QPlugin DLL
// （装进 <exe>/clientExtensions/，登记在本模块自己的列表里）。
// 与「工具扩展」（DllCaller 那条线）不是一套东西，只是都用 .ext 交付；
// 分流在 ExtensionLoader::loadAndInstall 里按 regulation 的 Type 字段做。
// 装载与 attach 必须在 GUI 线程：插件在 attachHost() 里要查注册表、往宿主控件树挂控件（见 core/DshHostPlugin.h）。

#include <QString>
#include <QStringList>

namespace ClientExtension
{
	// 目录：默认 <exe>/clientExtensions，环境变量 DSHHUB_CLIENT_EXTENSION_DIR 可覆盖。
	QString extensionDirectory();

	// 已装载的扩展名（取自 regulation 的 Name）。同时是防重复装载的依据。
	QStringList loadedNames();

	// 已安装的扩展名：扫 <dir>/<Name>/regulation.json5 —— 安装与移除都以它为判据；
	// 与 loadedNames() 的区别是它反映磁盘状态（重启后仍在、装载失败也在）。
	QStringList installedNames();

	// 移除一个已安装的扩展。返回值 = 目录是否真的删干净了：false 表示判据已删（不再算已安装、下次启动
	// 也不再装载）但仍有文件被占用，error 带回可直接展示的说明。
	bool remove(const QString& name, QString* error = nullptr);

	// regulation 的 Type 是否为客户端扩展类型（ClientExtension[Debug]，大小写不敏感）。
	bool isClientExtensionType(const QString& type);

	// 扫目录并逐个装载（取 <dir>/<Name>/ 下的 dll，main.dll 优先）；必须在 GUI 线程、且窗口与注册表就绪之后调用。
	// 返回本次成功装载的名字。
	QStringList loadAll();

	// 装载指定的一个 DLL；名字已在 loadedNames() 里则跳过（不重复 attach）。必须在 GUI 线程调用，
	// 失败原因写进 error（可选）。
	bool loadOne(const QString& dllPath, const QString& name, QString* error = nullptr);

	// 退出收尾（幂等）：关窗（DSHHub::closeEvent）与 aboutToQuit 各调一次，前者覆盖关窗、后者兜住
	// 不经窗口的退出（扩展自己 quit、系统关机）。做三件事：
	// (1) 同步调每个插件的 detachHost()（DirectConnection）复位接管、停子进程 —— 队列连接的通知
	//     要经事件循环投递，退出时随时停摆，不能当唯一自救路；(2) 兜底收回插件占的架空台；
	// (3) 把 loader 从 qApp 摘出去（setParent(nullptr)，故意不析构）：留着它 ~QApplication 就会
	//     卸 DLL、析构插件根对象，其 QProcess 析构卡死在已停的事件循环 + loader lock 上（实测的
	//     僵尸进程根因）。退出阶段不需要插件析构，DLL 交给 OS 回收。
	void shutdownForExit();
}
