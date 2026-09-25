#pragma once

// 宿主 exe 的 C 导出入口：插件运行期用 QLibrary::resolve 取地址来"拉"宿主对象（宿主无 .def/dllexport ⇒ 直接链接 LNK2019；插件自编一份 CommonRegistry 会得到第二个单例）。
// ABI：一律 extern "C" + 参数返回值只用 POD，动签名必须同步升 DSHHUB_HOST_ABI_VERSION；返回的 void* 是 QObject*，必须转成**全内联接口**再调（有 out-of-line 成员就撞回 LNK2019）。
// 查询只在主线程可用（注册表未加锁、装的是 UI 对象），非主线程返回 NULL 并首次 qWarning（投主线程见 ToolRequestDispatcher.h）；只读——刻意不导出 AddToRegistry，免得"谁负责析构"变成跨 DLL 所有权问题。

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

// C ABI 函数类型（定义在 src/core/HostExports.cpp）；返回的 void* = QObject*，不存在或已销毁时是 nullptr。
using DshHostAbiVersionFn = unsigned int (*)(void);
using DshHostRegistryFindFn = void* (*)(const char* index);

// index 常量：宿主登记与插件查询必须用同一份 —— 纯字符串无编译期检查，拼错只会静默拿到空对象（与"没登记""不在主线程"无从区分）。
namespace DshHostIndex
{
	inline constexpr const char* kMainWindow = "mainWindow";
	inline constexpr const char* kSidebar = "sidebar";
	inline constexpr const char* kTopBar = "topbar";
	// 样式表管理单例（ThemeManager），init() 末尾登记；插件转成 VirtualTheme* 调 ExternalApplyToWindow / ExternalReloadStyles。
	inline constexpr const char* kThemeManager = "themeManager";
	// 信号槽登记表单例（ConnectionManager），启动期登记；插件转成 VirtualConnectionManager* 登记/接管自己的连接。
	inline constexpr const char* kConnectionManager = "connectionManager";
}

namespace DshHost
{
	// 插件侧唯一取址入口：从宿主 exe 主模块取导出符号；用 applicationFilePath() 免引 <windows.h>，用 QLibrary::resolve 静态重载（成员版会被局部 QLibrary 析构卸载）。
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

	// 按 index 取宿主对象，返回 QPointer（长期持有也不会成野指针）；符号取不到 / index 为空 / 不在主线程 → 空。用法：qobject_cast<VirtualTopBar*>(DshHost::findObject("topbar"))。
	inline QPointer<QObject> findObject(const char* index)
	{
		const auto fn = registryFind();
		if (!fn || !index || !*index)
			return QPointer<QObject>();
		return QPointer<QObject>(static_cast<QObject*>(fn(index)));
	}
}
