#pragma once

// 客户端扩展（QPlugin DLL）的 attach 入口，也是宿主 ↔ 插件之间唯一的 C++ 接口。
// ⚠️ 必须全内联、无配套 .cpp（out-of-line 虚析构 ⇒ 真外部符号 ⇒ 插件链接期 LNK2019），且不能派生 QObject。
// 插件 → 宿主方向见 core/HostExports.h（宿主 exe 导出 C ABI）。

#include <QObject>

class DshHostPlugin
{
public:
	virtual ~DshHostPlugin() = default;

	// 挂载：ABI 握手 → 取宿主对象 → 建控件挂上去；调用时窗口与注册表已就绪
	// ⚠️ 会被多次调用（切主题会新建窗口再调一次），实现要能重复挂载、别累积状态
	virtual void attachHost() = 0;

	// 【可选】退场钩子，实现为一个槽 `detachHost()`
	// ⚠️ 元对象层字符串契约、非本接口虚方法：写成虚方法会改 vtable 布局，已发布插件会踩空槽位
	// 实现它 = 声明"之后宿主可安全 unload 我的 dll"，故必须同步删掉自己挂在宿主里的一切，不能 deleteLater
	// ⚠️ 接管了后端的扩展必须在这里复位 Takenover(false)，否则界面永久停在加载中
};

Q_DECLARE_INTERFACE(DshHostPlugin, "com.dshhub.DshHostPlugin/1.0")
