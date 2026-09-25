#pragma once

// 客户端扩展（QPlugin DLL）的 attach 入口，也是宿主 ↔ 插件之间唯一的 C++ 接口。
//
// 必须全内联、无配套 .cpp：out-of-line 虚析构是 key function ⇒ 真外部符号 ⇒
// 插件链接期 LNK2019；也不能派生 QObject（会拖进 staticMetaObject）。
// 插件 → 宿主那个方向见 core/HostExports.h（宿主 exe 导出 C ABI，插件运行期
// QLibrary::resolve 取地址），所以插件 DLL 的导入表里没有宿主符号。

#include <QObject>

class DshHostPlugin
{
public:
	virtual ~DshHostPlugin() = default;

	// 挂载到宿主：ABI 握手 → 取宿主对象 → 建自己的控件挂上去。
	// GUI 线程，且宿主保证调用时窗口与注册表都已就绪（可直接用 DshHost::findObject）。
	//
	// ⚠️ 会被调用**多次**：切主题会新建窗口，而插件实例在进程内保持存活
	// （qt_plugin_instance() 对同一个 DLL 返回同一个对象），新窗口会再调一次。所以
	// 实现要能在同一个实例上重复挂载 —— 上次挂的控件已随旧窗口销毁，别累积状态。
	virtual void attachHost() = 0;

	// 【可选】退场钩子：实现为一个槽 `detachHost()`。
	// 它是**元对象层的字符串契约，不是本接口的虚方法** —— 写成虚方法会改 vtable
	// 布局，已发布的插件会踩空槽位；用槽则老插件天然"没有"，宿主一查便知，零 ABI 变更。
	//
	// 实现它 = 声明"invoke 之后宿主可以安全 unload 我的 dll"：宿主随即
	// QPluginLoader::unload()、dll 解映射、扩展目录当场删净。所以里面必须**同步**删掉
	// 自己挂在宿主里的一切（按钮 / 窗口 / 定时器…），别用 deleteLater —— 卸载后那些
	// 对象的 vtable 指向已解映射的内存，碰一下就崩。
	//
	// 不实现 = 走兜底：扩展同样立刻从"已安装"消失、下次启动也不装载，只是当次那个 dll
	// 还被映射着删不掉，由下次启动的清扫收尾。
	//
	// 【契约】接管了后端的扩展（调过 VirtualApiHost::Takenover(true)）必须在这里**复位**
	// 接管状态（调 Takenover(false)）：那一刻插件还活着、还能通过接口做收尾，而之后
	// QPluginLoader::unload() 会把它从内存里解映射。不复位的后果不是崩溃，而是**没有报错
	// 的瘫痪** —— 标志仍是"已接管"、出站仍然发给一个已经解映射的接收端（QPointer 变空 ⇒
	// 每次都走 takenover-no-sink 失败），界面永久停在加载中，只能重启客户端。
	//
	// 宿主**不做**兜底（实验阶段的明确决定，见 misc/API_TAKEOVER_PLAN.zh-CN.md §4.2）：
	// 不实现 detachHost() 的插件不会被 unload（它仍被映射着，仍能正常服务），所以那种
	// 情况反而不需要复位；真正危险的就是"实现了 detachHost() 却忘了复位"这一种。
};

Q_DECLARE_INTERFACE(DshHostPlugin, "com.dshhub.DshHostPlugin/1.0")
