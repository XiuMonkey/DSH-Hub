#pragma once

// ------------------------------------------------------------------
// HostExports.h
// ------------------------------------------------------------------
// 宿主 exe 的 C 导出入口：插件自己来"拉"宿主对象。
//
// 为什么需要它：宿主是 Application、无 .def、无 dllexport，导出表本来是空的 ——
// 插件 include "CommonRegistry.h" 直接调 instance() 会 LNK2019；而"插件也编一份
// CommonRegistry.cpp"更糟，会得到【第二个单例】，插件的登记与查询和宿主静默分家。
// 反方向是通的：宿主在自己的映像里导出符号，插件运行期用 QLibrary::resolve 取地址，
// 链接期零依赖。
//
// ABI 规则（动签名 = 动 ABI，必须同步升 DSHHUB_HOST_ABI_VERSION）：
//   - 一律 extern "C"（导出表里是未修饰的名字），参数与返回值只用 POD。
//   - 返回的 void* 实际是 QObject*：插件自己转成**全内联接口**再调
//     （写法见 VirtualClass/VirtualTopBar.h）；接口一旦有 out-of-line 成员
//     就撞回 LNK2019。
//
// 线程：查询只在主线程可用（注册表未加锁，且表里是 UI 对象）。非主线程返回 NULL 并
//   （仅首次）打一条 qWarning。插件若在 Worker 线程里要用对象，自己把调用投到主
//   线程 —— 写法见 ToolRequestDispatcher.h。ABI 版本是常量，线程无关。
//
// 只读：刻意不导出 AddToRegistry —— 让插件往宿主全局表里塞对象，会把"谁负责析构"
//   变成跨 DLL 的所有权问题。
// ------------------------------------------------------------------

#include <QCoreApplication>
#include <QLibrary>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QtGlobal>

// 宿主 DshHubHostAbiVersion() 返回它；插件取到的值不一致就别再调后面的函数。
#define DSHHUB_HOST_ABI_VERSION 1u

// 导出符号名（QLibrary::resolve 走字符串，改 .cpp 里的定义时同步这里）
#define DSHHUB_HOST_SYM_ABI_VERSION   "DshHubHostAbiVersion"
#define DSHHUB_HOST_SYM_REGISTRY_FIND "DshHubHostRegistryFind"

// ---- C ABI 函数类型（定义在 src/core/HostExports.cpp）----
// 返回的 void* = QObject*；对象不存在或已销毁时是 nullptr。
using DshHostAbiVersionFn = unsigned int (*)(void);
using DshHostRegistryFindFn = void* (*)(const char* index);

// ---- index 常量：宿主登记与插件查询必须用同一份 ----
// index 是纯字符串、没有编译期检查，拼错只会静默拿到空对象（和"没登记""不在主线程"
// 长得一模一样，无从区分）。所以两侧都从这里取，别手写字面量。
namespace DshHostIndex
{
	inline constexpr const char* kMainWindow = "mainWindow";
	inline constexpr const char* kSidebar = "sidebar";
	inline constexpr const char* kTopBar = "topbar";
	// 样式表管理单例（ThemeManager）。它在 init() 末尾登记（那时 qss 已合成、
	// 对象已经可用），插件侧转成 VirtualTheme* 调 ExternalApplyToWindow（把当前主题
	// 挂到自己的窗口）或 ExternalReloadStyles（覆盖完 <exe>/styles/ 后让宿主重读）。
	inline constexpr const char* kThemeManager = "themeManager";
}

namespace DshHost
{
	// 插件侧唯一的取址入口：从当前进程的主模块（宿主 exe）取导出符号。
	// 用 applicationFilePath() 而不是 GetModuleHandle(NULL)：不必引 <windows.h>。
	// 用 QLibrary::resolve 的静态重载：它让库保持加载到进程结束（成员版本会被
	// 局部 QLibrary 对象的析构卸载掉）。
	inline QFunctionPointer resolveHostSymbol(const char* symbol)
	{
		const QString host = QCoreApplication::applicationFilePath();
		if (host.isEmpty() || !symbol)
			return nullptr;
		return QLibrary::resolve(host, symbol);
	}

	inline DshHostAbiVersionFn abiVersion()
	{
		static const auto fn = reinterpret_cast<DshHostAbiVersionFn>(
			resolveHostSymbol(DSHHUB_HOST_SYM_ABI_VERSION));
		return fn;
	}

	inline DshHostRegistryFindFn registryFind()
	{
		static const auto fn = reinterpret_cast<DshHostRegistryFindFn>(
			resolveHostSymbol(DSHHUB_HOST_SYM_REGISTRY_FIND));
		return fn;
	}

	// 按 index 取宿主对象。返回 QPointer —— 插件长期持有也不会成野指针。
	// 宿主符号取不到 / index 为空 / 当前不在主线程 → 返回空。
	//   if (auto* bar = qobject_cast<VirtualTopBar*>(DshHost::findObject("topbar")))
	//       bar->GetLayout()->addWidget(myWidget);
	inline QPointer<QObject> findObject(const char* index)
	{
		const auto fn = registryFind();
		if (!fn || !index || !*index)
			return QPointer<QObject>();
		return QPointer<QObject>(static_cast<QObject*>(fn(index)));
	}
}
