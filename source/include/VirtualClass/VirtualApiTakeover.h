#pragma once

// 后端接管（API takeover）的两个公共接口：宿主实现一个、客户端扩展实现一个。
//
// 目的：让客户端扩展（.ext 里 `Type: ClientExtension` 那条路线、进程内 QPlugin DLL）
// 取代内置 DSH 服务端 —— 接管时内置 DSH 进程被停掉，服务端逻辑与内容处理由扩展负责，
// 宿主 UI 沿用现有渲染管线（一行不改）。
//
// 与 VirtualClass/VirtualCommon.h 里那批接口同一套路（全内联、不派生 QObject、IID 转换、
// vtable 调用，插件侧零宿主符号）。四条 ABI 规则见 misc/DESIGN_NOTES.zh-CN.md:27-30：
//   · 全内联、无 out-of-line 成员（任何 out-of-line 成员都是真外部符号 ⇒ 插件 LNK2019）
//   · 虚方法只许**在末尾追加**（vtable 槽位 = 声明顺序，插件是独立编译的）
//   · 改 IID = 改 ABI（旧插件**静默**拿到 nullptr，功能无声消失）
//   · 参数可带 Qt 类型（两边共用同一份 Qt）
//
// 为什么单独一个头文件、而不是把虚方法加进 network/DshApiClient.h：那个类的 ctor/dtor/
// 方法全在 .cpp（out-of-line），插件一碰这个类型就 LNK2019。
//
// ── 两个方向的通讯形态（定案，见 misc/API_TAKEOVER_PLAN.zh-CN.md §2.6）──────────
//   插件 → 宿主：接口一（宿主 = DshApiClient；插件用注册表取址：DshHost::findObject(
//                DshHostIndex::kApiClient) → qobject_cast<VirtualApiHost*>）
//   宿主 → 插件：接口二（插件**根对象**实现，与 DshHostPlugin 同一条装载线；
//                宿主在 ClientExtension 装载时 cast 一次并登记到 kApiSink）
//   入站数据注入**不走接口**：插件用字符串 QMetaObject::invokeMethod 调 DSHHub 的槽
//   （可注入的槽清单与签名见方案文档 §2.6）。
//
// ── 出站请求的下发形态（接口二）──────────────────────────────────────────────
//   一元 RPC（callMethod / callMethodValue / respond 最终都汇到这里）：
//     method    = DSH 方法名原样（"session/create"、"session/page"、"$events/result" …）
//     argsJson  = 调用方交给 callMethod 的那份 payload 序列化的 JSON
//                 （= DSH 线上的 args 对象；respond 那次带 clientId / eventId / outcome）
//     扩展用 CompleteCall / FailCall 回填。回填的是**成功回调该拿到的那份 value**，
//     不是整个 server-response 信封。
//   流控制（只有两条，都是 fire-and-forget，不回填也无所谓）：
//     "$takeover/stream-open"     argsJson = {"endpoint":"session/follow","args":{…}}
//     "$takeover/stream-cancel"   argsJson = {"endpoint":"session/follow"}
//   ⚠️ 回填一个不认识的 rpcId（重复回填、fire-and-forget 的 id）只会记一条日志。
//   ⚠️ **开始喂数据的时机是 Takenover(true)** —— 接管后 baseUrl 是空的、宿主不会再调
//      openStreams()，所以那 8 个出站方法里没有一个"后端已就绪"的回执。
//
// ── 回填错误码词汇表（D7）────────────────────────────────────────────────────
// 已核实宿主上层**没有任何一处拿 error.code 做比较**，全部是 `.arg(code, message)`
// 拼进用户可见文本（MessageQuery.cpp:824、Sidebar.cpp:641/653、MessageHost.cpp:708/737、
// DSHHub.cpp:744 等多处）⇒ 扩展自定即可。宿主自己产生的码统一带 `takenover-` 前缀：
//   takenover-no-sink      接管态下取不到扩展的接收端（未装载 / 已被卸载）
//   takenover-timeout      扩展在时限内没有回填（见 DshApiClient 的 kTakeoverCallTimeoutMs）
//   takenover-released     扩展调 Takenover(false) 交还后端，当时仍挂着的请求
//   takenover-bad-result   CompleteCall 的 resultJson 不是合法 JSON
// 扩展自己产生的码建议同样带前缀（如 takenover-transport、codex-http-500），
// 便于一眼分辨"是扩展这一侧失败了"。

#include <qplugin.h>

// 接口一：宿主实现（挂在 DshApiClient 上），插件调用 —— 插件 → 宿主。
class VirtualApiHost
{
public:
	virtual ~VirtualApiHost() = default;

	// 拨动"后端已被接管"开关：true = 宿主不再走 HTTP/WebSocket，全部出站交给扩展；
	// false = 交还后端（内置 DSH 服务端**不会**因此自动重启，见下面的说明）。
	//
	// 返回 void（D0=A）⇒ 调用方无法从返回值得知成没成。宿主拒绝接管只有两种情形，都会记
	// qWarning 并保持原状态：① 注册表里没有实现接口二的扩展（kApiSink 未登记）；
	// ② 不在 GUI 线程。所以插件应当以"接管后能不能收到 OnOutboundRequest"自检。
	//
	// ⚠️ 宿主侧的一切动作都是幂等的：切主题会重建主窗口并对**同一个插件实例**再调一次
	//    attachHost()（见 core/DshHostPlugin.h），插件应当在那里重新取接口（旧指针只会
	//    变空）并再调一次 Takenover(true)。
	// ⚠️ 交还（false）时宿主会把"还挂着的接管请求"用 takenover-released 失败掉，但**不会**
	//    重启内置 DSH 服务端 —— 那个进程在接管时已经被停掉了。要恢复内置后端只能重启客户端，
	//    或触发一次服务端重启（设置里保存服务端设置 → ServerManager::restart()）。
	virtual void Takenover(bool on) = 0;

	// 回填一次出站请求：resultJson 是**成功回调该拿到的那份 value**（不是 server-response
	// 信封）。可以是 JSON 对象、数组，也可以是裸标量（如 12 / "x" / true）。
	// rpcId 不认识（重复回填 / fire-and-forget 的 id）⇒ 只记一条 qWarning。
	virtual void CompleteCall(const char* rpcId, const char* resultJson) = 0;

	// 回填失败：code / message 原样进 RpcError（上层只拼文本，不比较 code）。
	// code 为空时宿主用 takenover-error 兜底。调用方没提供 onError 时只记日志。
	virtual void FailCall(const char* rpcId, const char* code, const char* message) = 0;

	// ⚠️ 以后只许在末尾追加。
};
Q_DECLARE_INTERFACE(VirtualApiHost, "com.DSH_HUB.VirtualApiHost/1.0")

// 接口二：插件实现（挂在插件的**根对象**上，与 DshHostPlugin 同一个对象），宿主调用
// —— 宿主 → 插件。
//
// ⚠️ 插件类里必须写 `Q_INTERFACES(DshHostPlugin VirtualApiSink)`（多个 IID 空格分隔）：
//    宿主的 qobject_cast 走 moc 生成的 qt_metacast，它只认 Q_INTERFACES 列出的 IID。
//    漏了不会报错，只是永远 cast 出 nullptr —— 然后宿主在装载期就判定"这个扩展不能接管"。
class VirtualApiSink
{
public:
	virtual ~VirtualApiSink() = default;

	// 宿主把一次出站请求交给扩展（形态见文件头）。宿主只传**可序列化**的部分：两个回调是
	// std::function、不是 metatype，不能随请求交出去（既不能作信号参数、也不能跨 queued
	// 连接），所以它们留在宿主的 m_pending 表里，扩展回传结果后由宿主触发原来那个回调。
	//
	// ⚠️ 三个 const char* 只在**本次调用期间**有效，扩展要自己拷贝/转成 QString。
	// ⚠️ 在 GUI 线程同步调用；实现里别做耗时的事（阻塞的是整个界面）。
	virtual void OnOutboundRequest(const char* rpcId, const char* method, const char* argsJson) = 0;

	// ⚠️ 以后只许在末尾追加。
};
Q_DECLARE_INTERFACE(VirtualApiSink, "com.DSH_HUB.VirtualApiSink/1.0")
