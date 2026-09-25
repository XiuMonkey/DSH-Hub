#pragma once
#include <qplugin.h>
#include <qboxlayout.h>
// 接口里的参数只按指针传递，前向声明就够：宿主与插件各编一份，谁都不要依赖对方 exe 里的符号。
class QWidget;

class VirtualTopBar {
public:
	virtual ~VirtualTopBar() = default;
	virtual QHBoxLayout* GetLayout() = 0;
};

// ⚠️ 虚方法只许在末尾追加：宿主的 vtable 槽位 = 声明顺序，而插件 DLL 是独立编译的，插到中间会让已发布的插件全部调错位置。
class VirtualTheme {
public:
	virtual ~VirtualTheme() = default;
	virtual void ExternalApplyToWindow(QWidget* window) = 0;
	// 重新读一遍本地样式文件（exe 同目录 styles/*.qss 与 theme-*.json）并挂回所有顶层窗口；返回"重载后当前样式表非空"（false 基本等于文件没读到，便于插件覆盖完文件后自检）。
	virtual bool ExternalReloadStyles() = 0;
};

// VirtualWindow —— 宿主主窗口对外暴露的"窗口级"能力：把扩展自己的顶层窗口当成宿主弹窗（铺遮罩 + 居中显示）。
// 与 VirtualTheme 同一套路：宿主类实现它（Q_INTERFACES），插件把注册表里取到的 mainWindow 转成 VirtualWindow* —— 转换走 qt_metacast(IID)、调用走 vtable，插件侧零宿主符号。
class VirtualWindow {
public:
	virtual ~VirtualWindow() = default;

	// 铺遮罩 + 把 popup 居中并显示，两步背靠背完成（同 WindowFrame::showOverlayWithPopup，不会出现"遮罩先到、弹窗后到"）；⚠️ 调用前 popup 的尺寸必须已经定好，宿主按自己的中心给它定位。
	virtual void ExternalShowOverlay(QWidget* popup) = 0;

	// 收遮罩，必须与 ExternalShowOverlay 成对：遮罩在宿主窗口上只留一层、按 owner 记名（见 WindowFrame.h），只有最后一个 release 的才真正隐藏。
	virtual void ExternalHideOverlay(QWidget* popup) = 0;
};

// ------------------------------------------------------------------
// VirtualShell —— "架空原 UI"：把宿主整个客户区让给扩展自绘
// ------------------------------------------------------------------
// 与 VirtualTopBar / VirtualWindow 同一套路（全内联、IID 转换、vtable 调用，
// 插件侧零宿主符号），区别在语义：
//   VirtualTopBar / VirtualWindow 是"往宿主已有的东西上加 / 借宿主的壳"
//   VirtualShell 是"把宿主自己那套界面**整体让出去**"
//
// 宿主保证：
//   · 抢台成功后，原生控件树不可见、不接事件，但**不销毁** —— 还台立刻回来，
//     消息、滚动位置、输入内容都还在（不需要重新加载）。
//   · 让渡范围是**整个客户区**。宿主只保留窗口的非客户区行为（边缘缩放、
//     最大化、贴边吸附、系统菜单）。
//
// 本接口在宿主侧的全部实现就是转发给 ExtensionSystem/UiStage.h ——
// 宿主原有代码只多了"一个接口 + 几处 if"，让渡动作本身在 UiStage.cpp 里。
//
// ⚠️ 虚方法只许**在末尾追加**（宿主 vtable 槽位 = 声明顺序，插件独立编译）。
// ⚠️ 全内联、不派生 QObject、无 out-of-line 成员，否则插件链接期 LNK2019。
class VirtualShell
{
public:
	virtual ~VirtualShell() = default;

	// 抢台。owner 必须等于扩展的**安装目录名**（<exe>/clientExtensions/<Name>/）：
	// 宿主靠它做所有权校验，以及卸载时强制收台。不知道自己的名字不用硬编码 ——
	// 宿主在调用 attachHost() 之前会推一次可选槽 setHostIdentity(QString)
	// （元对象层的字符串契约，与 detachHost() 同一套路，零 ABI 变更）。
	//
	// 成功返回空舞台（已挂进窗口、已是当前可见面）；失败返回 nullptr。
	// 失败的正常情形：已被别人占着（同一时刻只允许一个 owner，**绝不静默顶掉**）、
	// owner 为空、不在 GUI 线程。同一 owner 重复调用是幂等的。
	//
	// ⚠️ 时机：必须写在 attachHost() 里。切主题会重建主窗口并对同一插件实例再调
	//    一次 attachHost()（见 DshHostPlugin.h），写在构造函数里会"切完主题就没了"。
	virtual QWidget* ExternalAcquireStage(const char* owner) = 0;

	// 还台。owner 不匹配一律拒绝并返回 false —— 防止被顶替的扩展在退场时把
	// 新 owner 的舞台收走。还台后原生界面立刻恢复。
	// ⚠️ 调用方必须在同一时刻把自己挂在舞台里的控件**同步**删干净
	//    （detachHost() 的既有契约）；舞台本身归宿主，不要也不能由扩展删除。
	virtual bool ExternalReleaseStage(const char* owner) = 0;

	// 自检：当前是否有人在架空着（第二个想抢台的扩展可以先问一下再决定退化）。
	virtual bool ExternalStageAcquired() = 0;

	// 自绘窗口条交给宿主的唯一信息：顶部哪一条算窗口拖动区（逻辑像素）。
	// top = 距窗口上沿，height = 高度。不调它就等于整个客户区都是客户区，
	// 窗口只能靠 Alt+Space 动。
	// 宿主会先排掉带动态属性 dshWindowControl=true 的控件（既有约定），
	// 所以扩展自己的最小化 / 关闭按钮照旧可点。
	virtual void ExternalSetCaptionBand(int top, int height) = 0;
};

class VirtualConnectionManager {
public:
	struct ConnectionGroup {
		QObject* Sender = nullptr;
		QObject* Receiver = nullptr;
		QByteArray mSignal;
		QByteArray mSlot;
	};
	virtual ~VirtualConnectionManager() = default;
	virtual void RegisterConnection(QString mIndex,ConnectionGroup mConnectionGroup)=0;
	virtual void PublicRemoveConnection(QString mIndex) = 0;
	virtual void SuspendConnection(QString mIndex) = 0;
	virtual void TakeoverConnection(QString mIndex, QObject* mObject, QByteArray mSlot) =0;
	virtual void Reconnect(QString mIndex)=0;
};

Q_DECLARE_INTERFACE(VirtualShell, "com.DSH_HUB.VirtualShell/1.0")
Q_DECLARE_INTERFACE(VirtualWindow, "com.DSH_HUB.VirtualWindow/1.0")
Q_DECLARE_INTERFACE(VirtualConnectionManager, "com.DSH_HUB.VirtualConnectionManager/1.0")
Q_DECLARE_INTERFACE(VirtualTopBar, "com.DSH_HUB.VirtualCommon/1.0")
Q_DECLARE_INTERFACE(VirtualTheme, "com.DSH_HUB.VirtualTheme/1.0")
