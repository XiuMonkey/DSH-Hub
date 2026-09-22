#include "core/HostExports.h"

#include "common/util/CommonRegistry.h"

#include <QCoreApplication>
#include <QString>
#include <QThread>

// HostExports.h 里那两个 C 入口的**定义**（只有宿主 exe 编这个文件）。

// extern "C" 保证导出表里是未修饰的名字。用显式 __declspec 而不是 Q_DECL_EXPORT：
// 后者是"给 Qt 自己建库用"的宏，展开随编译器/平台分支变化，末尾还有一段展开成空的
// 兜底（<Qt>/include/QtCore/qcompilerdetection.h），表达不了"这个 exe 必须导出"。
#if defined(Q_OS_WIN)
#  define DSHHUB_HOST_EXPORT extern "C" __declspec(dllexport)
#else
#  define DSHHUB_HOST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{
	// 没有 QCoreApplication 时（启动极早期 / 已析构）也一律拒绝 —— 那时"主线程"
	// 没有可信含义，Qt 自身状态也不可靠。
	bool onGuiThread()
	{
		const QCoreApplication* app = QCoreApplication::instance();
		if (!app)
			return false;
		return QThread::currentThread() == app->thread();
	}

	// 仅首次告警（插件可能每个请求都调一次），并把"该怎么做"写在告警里。
	void warnOffGuiThreadOnce(const char* symbol)
	{
		static bool warned = false;
		if (warned)
			return;
		warned = true;
		qWarning("DshHubHost: %s called from a non-GUI thread, refused. "
			"The registry is main-thread only (UI objects); marshal the call "
			"to the GUI thread yourself, e.g. QMetaObject::invokeMethod with "
			"Qt::QueuedConnection (see ToolRequestDispatcher.h).",
			symbol);
	}
}

// ABI 版本：常量，线程无关，插件启动时用它握手。
DSHHUB_HOST_EXPORT unsigned int DshHubHostAbiVersion(void)
{
	return DSHHUB_HOST_ABI_VERSION;
}

// 按 index 查宿主对象（void* 实际是 QObject*）。
// index 为空 / 未登记 / 对象已销毁 / 非主线程 → nullptr。
DSHHUB_HOST_EXPORT void* DshHubHostRegistryFind(const char* index)
{
	if (!index || !*index)
		return nullptr;
	if (!onGuiThread())
	{
		warnOffGuiThreadOnce(DSHHUB_HOST_SYM_REGISTRY_FIND);
		return nullptr;
	}

	// 返回裸指针是刻意的（C ABI 不传 QPointer）。QPointer 在对象销毁后自动为空，
	// data() 本就是 nullptr，无需额外判活 —— 插件侧应立刻用 QPointer 接住。
	return CommonRegistry::instance().FindFromRegistry(QString::fromUtf8(index)).data();
}

#undef DSHHUB_HOST_EXPORT

// 编译期签名校验：定义必须与头文件里的 typedef 一致，否则插件会按错误签名去调
// （栈上直接出事）。只是取地址做类型绑定，不产生代码。
namespace
{
	[[maybe_unused]] const DshHostAbiVersionFn kAbiVersionCheck = &DshHubHostAbiVersion;
	[[maybe_unused]] const DshHostRegistryFindFn kRegistryFindCheck = &DshHubHostRegistryFind;
}