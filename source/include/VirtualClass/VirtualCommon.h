#pragma once
#include <qplugin.h>
#include <qboxlayout.h>
// 参数只按指针传递，前向声明就够 —— 宿主与插件各编一份，互不依赖对方 exe 里的符号
class QWidget;

class VirtualTopBar {
public:
	virtual ~VirtualTopBar() = default;
	virtual QHBoxLayout* GetLayout() = 0;
};

// ⚠️ 虚方法只许在末尾追加：vtable 槽位 = 声明顺序，插件 DLL 独立编译，插中间会让旧插件调错位置
class VirtualTheme {
public:
	virtual ~VirtualTheme() = default;
	virtual void ExternalApplyToWindow(QWidget* window) = 0;
	// 重读 exe 同目录 styles/*.qss 与 theme-*.json 并挂回所有顶层窗口
	virtual bool ExternalReloadStyles() = 0;
};

// 把扩展自己的顶层窗口当成宿主弹窗（铺遮罩 + 居中显示）
class VirtualWindow {
public:
	virtual ~VirtualWindow() = default;

	// 铺遮罩 + 居中显示，两步背靠背；⚠️ 调用前 popup 尺寸必须已定好
	virtual void ExternalShowOverlay(QWidget* popup) = 0;

	// 收遮罩，须与 ExternalShowOverlay 成对：遮罩只留一层，最后一个 release 才真隐藏
	virtual void ExternalHideOverlay(QWidget* popup) = 0;
};

// 架空原 UI：把宿主整个客户区让给扩展自绘；原生控件树不销毁，还台即恢复
// ⚠️ 虚方法只许末尾追加；⚠️ 全内联、不派生 QObject，否则插件链接期 LNK2019
class VirtualShell
{
public:
	virtual ~VirtualShell() = default;

	// 抢台，owner 必须是扩展安装目录名；⚠️ 只能写在 attachHost() 里（切主题会重调）
	virtual QWidget* ExternalAcquireStage(const char* owner) = 0;

	// 还台，owner 不匹配一律拒绝；⚠️ 调用方须同步删干净自己挂在舞台里的控件
	virtual bool ExternalReleaseStage(const char* owner) = 0;

	// 当前是否有人在架空着
	virtual bool ExternalStageAcquired() = 0;

	// 顶部哪一条算窗口拖动区（top = 距上沿，height = 高度）；不调则只能靠 Alt+Space 动窗口
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
