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

	// 退出路径的收尾（幂等）。主窗口关窗（DSHHub::closeEvent）与 QCoreApplication::aboutToQuit 各调一次 ——
	// 前者覆盖"用户关窗"这条最常见的路，后者兜住扩展自己 quit()、系统关机那些不经窗口的退出。
	//
	// 做三件事：
	//   ① 同步调每个插件的 detachHost()（元对象槽，DirectConnection）：插件在这里复位接管、停自己的子进程。
	//      只"通知"不"收尾"是不够的 —— 插件通常靠订阅关窗信号自救，而那条路要经事件循环投递（队列连接），
	//      退出时事件循环随时会停，投出去的通知可能永远不执行。
	//   ② 兜底收回插件占的架空台（幂等；插件崩了或忘了 release 会留下 vtable 已解映射的控件）。
	//   ③ 把 loader 从 qApp 的 children 里摘出去（setParent(nullptr)，**故意不析构**）。
	//
	// ③ 是本函数存在的理由：只要 loader 还挂在 qApp 下，进程退出时 ~QApplication 就会删它 → 卸插件 DLL →
	// 析构插件根对象 → 插件里 QProcess 的析构卡在管道取消上。而那一刻事件循环已经停了、loader lock 还在
	// 手里，这个等待永远不返回 —— 实测症状就是"窗口没了、线程掉到 1 个、进程不退、DLL 还被锁着"的僵尸进程。
	// 退出阶段本来也不需要做插件的析构：DLL 交给 OS 在进程终止时回收。
	void shutdownForExit();
}
