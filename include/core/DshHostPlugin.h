#pragma once

// ------------------------------------------------------------------
// DshHostPlugin.h
// ------------------------------------------------------------------
// 客户端扩展（QPlugin DLL）的 attach 入口 —— 宿主 ↔ 插件之间唯一的 C++ 接口。
//
// 两个方向都不需要 dllimport/dllexport 配对：
//   宿主 → 插件：QPluginLoader 按 C 名字取实例（qt_plugin_instance），再转成本接口；
//   插件 → 宿主：core/HostExports.h 的 C 导出，运行期 QLibrary::resolve 取地址。
//   ⇒ 插件 DLL 的导入表里没有宿主符号。
//
// ⚠️ 本接口必须**全内联、没有配套 .cpp**：out-of-line 的虚析构是 key function ⇒
//    真外部符号 ⇒ 插件链接期 LNK2019。也不能派生自 QObject（会拖进 staticMetaObject）。
//    写法照 VirtualClass/VirtualTopBar.h 来。
//
// ⚠️ attachHost() 可能被调用**多次**：切主题会新建窗口，而插件实例在进程内保持存活
//    （qt_plugin_instance() 对同一个 DLL 返回同一个对象），新窗口会再调一次。所以实现
//    要能在同一个实例上重复挂载：上次挂的控件已随旧窗口销毁，别累积状态。
//
// 调用时机：GUI 线程，且在窗口与注册表都就绪之后
//   （见 ExtensionSystem/ClientExtension.h）—— 所以里面可以直接用 DshHost::findObject()。
// ------------------------------------------------------------------

#include <QObject>

class DshHostPlugin
{
public:
	virtual ~DshHostPlugin() = default;

	// 挂载到宿主：ABI 握手 → 取宿主对象 → 建自己的控件挂上去。
	// 全程只在 GUI 线程做（注册表与 UI 都只在那里可用）。
	virtual void attachHost() = 0;

	// ------------------------------------------------------------------
	// 【可选】退场钩子：detachHost()
	// ------------------------------------------------------------------
	// 宿主在**移除**本扩展时会先查元对象里有没有 `detachHost()` 槽 ——
	// 纯字符串契约，**不是本接口的虚方法**（写成虚方法会改 vtable 布局，已发布的
	// 老插件会踩空槽位；用槽则老插件天然"没有"，宿主一查便知，零 ABI 变更）。
	//
	// 实现了它就等于向宿主声明："invoke 之后，你可以安全卸载我的 dll"。宿主随即
	// QPluginLoader::unload()，dll 从进程里解映射、扩展目录当场删干净。
	// 所以实现里必须**同步**删掉自己挂在宿主里的一切（按钮、窗口、定时器…）：
	//     class MyPlugin : public QObject, public DshHostPlugin {
	//         Q_OBJECT
	//     public slots:
	//         void detachHost() { delete m_button; delete m_window; }   // 别用 deleteLater
	//     };
	// 之后宿主再也不会（也不能）用到插件的任何对象 —— 卸载后那些对象的 vtable
	// 指向已解映射的内存，碰一下就崩。
	//
	// 没实现它 = 宿主走兜底路径：扩展同样立刻从"已安装"里消失、下次启动也不会装载，
	// 只是当次那个 dll 文件还被映射着、删不掉，由下次启动的清扫收尾。
	// ------------------------------------------------------------------
};

Q_DECLARE_INTERFACE(DshHostPlugin, "com.dshhub.DshHostPlugin/1.0")
