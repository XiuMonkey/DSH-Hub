#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <qboxlayout.h>
#include <qplugin.h>
// 参数只按指针传递，前向声明就够 —— 宿主与插件各编一份，互不依赖对方 exe 里的符号
class QWidget;

class VirtualTopBar {
public:
	virtual ~VirtualTopBar() = default;
	virtual QHBoxLayout* GetLayout() = 0;
};

// 侧栏宿主接口：把底部那一排图标按钮的横向布局交给扩展，让扩展能挂自己的入口按钮。
//
// ⚠️ 与 VirtualTopBar::GetLayout() 同一条教训（Sidebar.cpp 原来就是把布局写成构造里的局部
//    变量，外面拿不到）：布局必须是 Sidebar 的**类级成员**。
// ⚠️ 指针只在侧栏存活期间有效（切主题会重建主窗口、连带换掉这个对象，扩展每次 attachHost()
//    都要重新 findObject + cast，别缓存）。
// 排布：底排顺序是「设置 | 插件 | 主题 | 扩展 | 15px 间距 | stretch(1)」，所以
//    · addWidget(w)        → 落在 stretch 之后，贴侧栏**最右**；
//    · insertWidget(旧按钮的 indexOf, w) → 插进那排图标之间（宿主自身的顺序别改）。
class VirtualSidebar {
public:
	virtual ~VirtualSidebar() = default;
	virtual QHBoxLayout* GetNavLayout() = 0;
};

// 虚方法只许在末尾追加：vtable 槽位 = 声明顺序，插件 DLL 独立编译，插中间会让旧插件调错位置
class VirtualTheme {
public:
	virtual ~VirtualTheme() = default;
	virtual void ExternalApplyToWindow(QWidget* window) = 0;
	// 重读 exe 同目录 styles/*.qss 与 theme-*.json 并挂回所有顶层窗口
	virtual bool ExternalReloadStyles() = 0;

	// 【末尾追加】当前是不是深色主题（扩展自绘的设置界面要显示"当前：浅色/深色"）。
	virtual bool ExternalIsDarkMode() = 0;

	// 【末尾追加】切换主题：与侧栏那颗主题图标按钮**完全同一条路**（过渡弹窗 → 重建主窗口 →
	// 写进 AppearanceSetting.json）。currentWindow 传当前主窗口 —— 宿主自己也是这么调的
	// （DSHHub::toggleTheme → ThemeManager::switchTheme(this)），扩展可以传 findObject(kMainWindow)。
	// ⚠️ 副作用要清楚：它会**销毁并重建主窗口** ⇒ 扩展挂在旧窗口上的控件/弹窗会随之消失
	//    （宿主自己的弹窗也一样：新窗口构造时才重建）。调用方要在之后重挂自己的 UI。
	virtual void ExternalSwitchTheme(QWidget* currentWindow) = 0;
};

// 主窗口宿主接口：接收扩展的顶层窗口当作宿主弹窗，以及扩展灌入的会话 / 工作区数据
class VirtualMain {
public:
	virtual ~VirtualMain() = default;

	// 铺遮罩 + 居中显示，两步背靠背；调用前 popup 尺寸必须已定好
	virtual void ExternalShowOverlay(QWidget* popup) = 0;

	// 收遮罩，须与 ExternalShowOverlay 成对：遮罩只留一层，最后一个 release 才真隐藏
	virtual void ExternalHideOverlay(QWidget* popup) = 0;

	// 入站数据注入（后端接管专用）：12 个方法是 DshApiClient 同名信号的替代入口 —— 正常路径由信号
	// 驱动，接管后信号没人发，扩展改调这里；宿主实现转发到 DSHHub 同名私有槽，槽保持 private。
	// 必须在 GUI 线程同步调用；以后只许在末尾追加（方法名与签名一发布就是 ABI）。

	// 连接与传输：让 UI 认为已连接、一帧 mux 消息（会话事件 / 审批 / 提问，入站主力）、传输层错误
	virtual void HandleConnected() = 0;
	virtual void ForwardMuxFrame(const QJsonObject& frame) = 0;
	virtual void HandleTransportError(const QString& context, const QString& message) = 0;

	// 会话：follow 快照（首屏历史 + 游标）、快照自带的投影（小灰字）、control baseline、实时投影帧
	virtual void HandleSessionSnapshot(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore) = 0;
	virtual void HandleSessionProjections(const QString& sessionId, int asOfSeq, const QJsonObject& values) = 0;
	virtual void HandleSessionControlBaseline(const QJsonObject& projectionsBySession) = 0;
	virtual void HandleSessionProjectionChanged(const QString& sessionId, const QString& key, const QJsonValue& value, int seq) = 0;

	// 工作区：baseline（清单 + 归档集合）与四个增量帧；order / archived 都是整体替换
	virtual void HandleWorkspaceSnapshot(const QJsonArray& items, const QJsonArray& archivedSessionIds) = 0;
	virtual void HandleWorkspaceUpserted(const QJsonObject& workspace) = 0;
	virtual void HandleWorkspaceRemoved(const QString& workspaceId) = 0;
	virtual void HandleWorkspaceReordered(const QStringList& workspaceIds) = 0;
	virtual void HandleWorkspaceArchiveChanged(const QJsonArray& archivedSessionIds) = 0;

	// 侧栏会话标题刷新：回合收尾之外的另一条触发路。重拉标题但**不重选会话** —— refreshSessions
	// 拿到列表后会 emit initialSessionReady（自动选中的是"第一个非 running 的会话"），接管态下再走
	// 那条会把用户正在看的会话切走、还会打断流式输出。幂等、无副作用，可重复调。
	virtual void ExternalRefreshSessionTitles() = 0;
};

// 架空原 UI：把宿主整个客户区让给扩展自绘；原生控件树不销毁，还台即恢复
// 虚方法只许末尾追加；全内联、不派生 QObject，否则插件链接期 LNK2019
class VirtualShell
{
public:
	virtual ~VirtualShell() = default;

	// 抢台，owner 必须是扩展安装目录名；只能写在 attachHost() 里（切主题会重调）
	virtual QWidget* ExternalAcquireStage(const char* owner) = 0;

	// 还台，owner 不匹配一律拒绝；调用方须同步删干净自己挂在舞台里的控件
	virtual bool ExternalReleaseStage(const char* owner) = 0;

	// 当前是否有人在架空着
	virtual bool ExternalStageAcquired() = 0;

	// 顶部哪一条算窗口拖动区（top = 距上沿，height = 高度）；不调则只能靠 Alt+Space 动窗口
	virtual void ExternalSetCaptionBand(int top, int height) = 0;
};

// 信号槽登记表（插件可经 kConnectionManager 取到后 qobject_cast）。
// 接管走 TakeoverConnection（匿名，只凭 index）；新增订阅走 ProtectedRegisterConnection（白名单），
// 刻意不上 RegisterConnection —— 它要求调用方自供 sender + signal，等于交出宿主全部信号面。
class VirtualConnectionManager {
public:
	struct ConnectionGroup {
		QObject* Sender = nullptr;
		QObject* Receiver = nullptr;
		QByteArray mSignal;
		QByteArray mSlot;
	};
	virtual ~VirtualConnectionManager() = default;
	virtual void PublicRemoveConnection(QString mIndex) = 0;
	virtual void SuspendConnection(QString mIndex) = 0;
	virtual void TakeoverConnection(QString mIndex, QObject* mObject, QByteArray mSlot) = 0;
	virtual void Reconnect(QString mIndex) = 0;

	// 新增订阅宿主公开信号：signal 是原生签名串（"2clearRequested()"），不在白名单会被宿主拒绝
	virtual void ProtectedRegisterConnection(QString index, const QObject* sender, QByteArray signal, const QObject* receiver, const char* slot) = 0;

	// 以后只许在末尾追加。
};

// 消息区宿主接口：把"正在载入会话"这层提示的收放讲清楚。宿主只有"控件缓存命中"与"历史出错"
// 两条路会收这层提示，而接管态首屏历史是扩展喂的（只 emit contentReady），不替宿主收就得等它 6 秒看门狗。
class VirtualMessageHost {
public:
	virtual ~VirtualMessageHost() = default;

	// 亮"正在载入会话"提示（切会话时宿主自己也会亮；扩展一般不需要主动调）
	virtual void ExternalShowSessionLoading() = 0;

	// 收掉那层提示。接管态下**每次注入完 follow 快照都要调**（幂等，重复调无副作用）
	virtual void ExternalHideSessionLoading() = 0;
};

// 多语言宿主接口：让扩展自绘的设置界面也能读/改语言 —— 与宿主设置窗口「外观」页那个语言下拉框
// 走同一套实现（Translation 命名空间的自由函数），所以行为一致：换 translator → Qt 给所有控件发
// LanguageChange（接了 changeEvent 的自己刷新）→ 剩下的固定文案按快照就地换 → 再通知非控件对象。
// 实现类 = TranslationNotifier 单例（登记名 kTranslationNotifier，见 DshHostIndex）。
class VirtualTranslation {
public:
	virtual ~VirtualTranslation() = default;

	// 当前语言代码；**空串 = 跟随系统**（与设置里存的值同义）
	virtual QString ExternalLanguage() = 0;
	// 可选语言代码（不含"跟随系统"，它在界面里是单独一项）
	virtual QStringList ExternalLanguageCodes() = 0;
	// 语言代码 → 展示名（该语言自己的写法，如 "English" / "日本語"）；未知代码回空串
	virtual QString ExternalLanguageName(const QString& code) = 0;
	// 立即切换语言（免重启）并写进设置；返回是否真的换了。
	// ⚠️ 缺 .qm 时会退回源语言并返回 false —— 调用方应把这件事**显示给用户**（不是弹错误框）。
	virtual bool ExternalSetLanguage(const QString& code) = 0;
};

Q_DECLARE_INTERFACE(VirtualShell, "com.DSH_HUB.VirtualShell/1.0")
Q_DECLARE_INTERFACE(VirtualMain, "com.DSH_HUB.VirtualWindow/1.0")
Q_DECLARE_INTERFACE(VirtualConnectionManager, "com.DSH_HUB.VirtualConnectionManager/1.0")
Q_DECLARE_INTERFACE(VirtualTopBar, "com.DSH_HUB.VirtualCommon/1.0")
Q_DECLARE_INTERFACE(VirtualSidebar, "com.DSH_HUB.VirtualSidebar/1.0")
Q_DECLARE_INTERFACE(VirtualTheme, "com.DSH_HUB.VirtualTheme/1.0")
Q_DECLARE_INTERFACE(VirtualTranslation, "com.DSH_HUB.VirtualTranslation/1.0")
Q_DECLARE_INTERFACE(VirtualMessageHost, "com.DSH_HUB.VirtualMessageHost/1.0")
