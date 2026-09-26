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

// ⚠️ 虚方法只许在末尾追加：vtable 槽位 = 声明顺序，插件 DLL 独立编译，插中间会让旧插件调错位置
class VirtualTheme {
public:
	virtual ~VirtualTheme() = default;
	virtual void ExternalApplyToWindow(QWidget* window) = 0;
	// 重读 exe 同目录 styles/*.qss 与 theme-*.json 并挂回所有顶层窗口
	virtual bool ExternalReloadStyles() = 0;
};

// 主窗口宿主接口：接收扩展的顶层窗口当作宿主弹窗，以及扩展灌入的会话 / 工作区数据
class VirtualMain {
public:
	virtual ~VirtualMain() = default;

	// 铺遮罩 + 居中显示，两步背靠背；⚠️ 调用前 popup 尺寸必须已定好
	virtual void ExternalShowOverlay(QWidget* popup) = 0;

	// 收遮罩，须与 ExternalShowOverlay 成对：遮罩只留一层，最后一个 release 才真隐藏
	virtual void ExternalHideOverlay(QWidget* popup) = 0;

	// 入站数据注入（后端接管专用）：以下 12 个是 DshApiClient 同名信号的替代入口 —— 正常路径由信号
	// 驱动，接管后信号没人发，扩展改调这里。宿主侧实现就是转发到 DSHHub 的同名私有槽，槽保持
	// private，本接口是入站数据的唯一入口。
	// ⚠️ 必须在 GUI 线程同步调用（宿主直接改界面控件，没有排队）；实现里别做耗时的事。
	// ⚠️ 以后只许在末尾追加 —— 方法名与签名一发布就是 ABI，改一个字老插件调错槽位。

	// 连接与传输：让 UI 认为已连接、一帧 mux 消息（会话事件 / 审批 / 提问，入站主力）、传输层错误
	virtual void HandleConnected() = 0;
	virtual void ForwardMuxFrame(const QJsonObject& frame) = 0;
	virtual void HandleTransportError(const QString& context, const QString& message) = 0;

	// 会话：follow 快照（首屏历史 + 游标）、快照自带的投影（小灰字）、control baseline、实时投影帧
	virtual void HandleSessionSnapshot(const QString& sessionId, int cursor, const QJsonArray& records,
		bool hasMore) = 0;
	virtual void HandleSessionProjections(const QString& sessionId, int asOfSeq, const QJsonObject& values) = 0;
	virtual void HandleSessionControlBaseline(const QJsonObject& projectionsBySession) = 0;
	virtual void HandleSessionProjectionChanged(const QString& sessionId, const QString& key,
		const QJsonValue& value, int seq) = 0;

	// 工作区：baseline（清单 + 归档集合）与四个增量帧；order / archived 都是整体替换
	virtual void HandleWorkspaceSnapshot(const QJsonArray& items, const QJsonArray& archivedSessionIds) = 0;
	virtual void HandleWorkspaceUpserted(const QJsonObject& workspace) = 0;
	virtual void HandleWorkspaceRemoved(const QString& workspaceId) = 0;
	virtual void HandleWorkspaceReordered(const QStringList& workspaceIds) = 0;
	virtual void HandleWorkspaceArchiveChanged(const QJsonArray& archivedSessionIds) = 0;
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

// 信号槽登记表（插件可经 kConnectionManager 取到后 qobject_cast）。
//
// ⚠️ 宿主侧信号与槽**默认匿名**：插件不需要（也拿不到）信号名，只凭 index 就能把接收端换成
//    自己的 —— 那是 TakeoverConnection，匿名接管机制的本体。
// ⚠️ 这里**刻意没有 RegisterConnection**：它要求调用方自己提供 sender + signal，等于把
//    "连宿主任意信号"的能力交出去（拿到 kSidebar 就能连它全部信号，且无从区分哪一个被准了）。
//    插件要**新增订阅**（而非接管已有连接）只有下面这一条路：ProtectedRegisterConnection，
//    由宿主侧按**信号白名单**校验 signal —— 不在名单里的静默拒绝。
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
	virtual void TakeoverConnection(QString mIndex, QObject* mObject, QByteArray mSlot) =0;
	virtual void Reconnect(QString mIndex)=0;

	// 新增订阅一个宿主**公开**的信号：宿主侧先查白名单（ConnectionManager.h 的 m_publicSignals），
	// signal 不在名单里就拒绝（不注册、不连接，只在宿主日志里留一行警告 —— 不静默）。
	// ⚠️ 比对的是 signal 本身；index 由调用方自起名（建议带扩展名前缀），只用于事后取消订阅。
	// ⚠️ signal 是原生签名串（"2clearRequested()"），按 QMetaObject 的查找规则；
	//    白名单里存的也是同一种串，改了宿主信号名必须同步改名单。
	virtual void ProtectedRegisterConnection(QString index, const QObject* sender, QByteArray signal,
		const QObject* receiver, const char* slot) = 0;

	// ⚠️ 以后只许在末尾追加。
};

// 消息区宿主接口：扩展灌完历史/事件后，负责把"正在载入会话"这层提示的收放讲清楚。
// 为什么需要它：宿主侧只有"控件缓存命中"与"历史出错"两条路会收这层提示，而**接管态下
// 首屏历史是扩展喂的**（follow 快照成功那条路只 emit contentReady，收的是启动遮罩）——
// 不替宿主收，它就只能等自己 6 秒的看门狗兜底（日志里的 loading overlay watchdog fired）。
class VirtualMessageHost {
public:
	virtual ~VirtualMessageHost() = default;

	// 亮"正在载入会话"提示（切会话时宿主自己也会亮；扩展一般不需要主动调）
	virtual void ExternalShowSessionLoading() = 0;

	// 收掉那层提示。接管态下**每次注入完 follow 快照都要调**（幂等，重复调无副作用）
	virtual void ExternalHideSessionLoading() = 0;
};

Q_DECLARE_INTERFACE(VirtualShell, "com.DSH_HUB.VirtualShell/1.0")
Q_DECLARE_INTERFACE(VirtualMain, "com.DSH_HUB.VirtualWindow/1.0")
Q_DECLARE_INTERFACE(VirtualConnectionManager, "com.DSH_HUB.VirtualConnectionManager/1.0")
Q_DECLARE_INTERFACE(VirtualTopBar, "com.DSH_HUB.VirtualCommon/1.0")
Q_DECLARE_INTERFACE(VirtualTheme, "com.DSH_HUB.VirtualTheme/1.0")
Q_DECLARE_INTERFACE(VirtualMessageHost, "com.DSH_HUB.VirtualMessageHost/1.0")
