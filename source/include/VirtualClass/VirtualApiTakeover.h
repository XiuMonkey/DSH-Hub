#pragma once

// 后端接管（API takeover）的两个公共接口：宿主实现接口一（挂在 DshApiClient 上），客户端扩展
// 实现接口二（挂在插件根对象上）。接管时内置 DSH 进程被停掉，全部出站交给扩展负责，宿主 UI
// 沿用现有渲染管线（一行不改）。入站数据注入不在本文件 —— 见 VirtualClass/VirtualCommon.h 的
// VirtualMain（DSHHub 实现，宿主侧转发到同名私有槽）。
//
// 四条 ABI 规则（misc/DESIGN_NOTES.zh-CN.md:27-30）：
//   · 全内联、无 out-of-line 成员 —— 任何 out-of-line 成员都是真外部符号 ⇒ 插件 LNK2019。
//   · 虚方法只许在末尾追加 —— vtable 槽位 = 声明顺序，而插件是独立编译的。
//   · 改 IID = 改 ABI —— 旧插件会静默拿到 nullptr，功能无声消失。
//   · 参数可带 Qt 类型 —— 两边共用同一份 Qt。
// 之所以单独开这个头文件、而不把虚方法加进 network/DshApiClient.h：那个类的 ctor/dtor/方法
// 全在 .cpp（out-of-line），插件一碰这个类型就 LNK2019。
//
// 通讯形态（见 misc/API_TAKEOVER_PLAN.zh-CN.md §2.6）：插件 → 宿主走接口一，插件经
// DshHost::findObject(DshHostIndex::kApiClient) 取址后 qobject_cast；宿主 → 插件走接口二，
// 宿主在 ClientExtension 装载时 cast 一次并登记到 kApiSink。入站数据注入走 VirtualMain：插件经
// DshHost::findObject(DshHostIndex::kMainWindow) 取址后 qobject_cast。
//
// 出站请求：一元 RPC 汇成 (method, argsJson)，扩展用 CompleteCall / FailCall 回填成功回调该
// 拿到的 value（不是 server-response 信封）；流控制只有 "$takeover/stream-open" 与
// "$takeover/stream-cancel" 两条，都是 fire-and-forget。
// ⚠️ 开始喂数据的时机是 Takenover(true)：接管后 baseUrl 为空、宿主不再调 openStreams()，
//    那 8 个出站方法里没有一个"后端已就绪"的回执。
// ⚠️ 回填不认识的 rpcId（重复回填 / fire-and-forget 的 id）只记一条日志。
//
// 回填错误码：已核实宿主上层没有任何一处比较 error.code，全部拼进用户可见文本，扩展自定即可。
// 宿主自己产生的码统一带 takenover- 前缀：no-sink / timeout / released / bad-result。

#include <qplugin.h>

// 接口一：宿主实现（挂在 DshApiClient 上），插件调用 —— 插件 → 宿主。
class VirtualApiHost
{
public:
	virtual ~VirtualApiHost() = default;

	// 拨动"后端已被接管"开关：true = 宿主不再走 HTTP/WebSocket，全部出站交给扩展；false =
	// 交还后端（内置 DSH 服务端不会因此自动重启）。
	// 返回 void（D0=A）⇒ 调用方无法从返回值得知成没成；宿主拒绝接管只有"注册表里没有实现
	// 接口二的扩展"和"不在 GUI 线程"两种情形，都会记 qWarning 并保持原状态。
	// ⚠️ 宿主侧动作都是幂等的：切主题会重建主窗口并对同一个插件实例再调一次 attachHost()
	//   （见 core/DshHostPlugin.h），插件应在那里重新取接口并再调一次 Takenover(true)。
	// ⚠️ 交还时宿主用 takenover-released 失败掉仍挂着的接管请求，但不会重启内置服务端 —— 要
	//   恢复内置后端只能重启客户端，或在设置里保存服务端设置触发 ServerManager::restart()。
	virtual void Takenover(bool on) = 0;

	// 回填一次出站请求：resultJson 是成功回调该拿到的那份 value（不是 server-response 信封），
	// 可以是对象、数组或裸标量。rpcId 不认识（重复回填 / fire-and-forget 的 id）只记 qWarning。
	virtual void CompleteCall(const char* rpcId, const char* resultJson) = 0;

	// 回填失败：code / message 原样进 RpcError（上层只拼文本、不比较 code）；code 为空时宿主
	// 用 takenover-error 兜底。调用方没提供 onError 时只记日志。
	virtual void FailCall(const char* rpcId, const char* code, const char* message) = 0;

	// 接管态自检口：Takenover(bool) 无回执，调完读它确认有没有被接受；只报状态不区分接管者
	virtual bool IsTakenover() const = 0;

	// ⚠️ 以后只许在末尾追加。
};
Q_DECLARE_INTERFACE(VirtualApiHost, "com.DSH_HUB.VirtualApiHost/1.0")

// 接口二：插件实现（挂在插件的根对象上，与 DshHostPlugin 同一个对象），宿主调用 —— 宿主 → 插件。
// ⚠️ 插件类里必须写 `Q_INTERFACES(DshHostPlugin VirtualApiSink)`（多个 IID 空格分隔）：宿主的
//   qobject_cast 走 moc 生成的 qt_metacast，它只认 Q_INTERFACES 列出的 IID。漏了不报错，只是
//   永远 cast 出 nullptr，然后宿主在装载期就判定"这个扩展不能接管"。
class VirtualApiSink
{
public:
	virtual ~VirtualApiSink() = default;

	// 宿主把一次出站请求交给扩展（形态见文件头）。宿主只传可序列化的部分：两个回调是
	// std::function、不是 metatype，不能随请求交出去（既不能作信号参数、也不能跨 queued 连接），
	// 所以它们留在宿主的 m_pending 表里，等扩展回传结果后由宿主触发原来那个回调。
	// ⚠️ 三个 const char* 只在本次调用期间有效，扩展要自己拷贝/转成 QString。
	// ⚠️ 在 GUI 线程同步调用；实现里别做耗时的事（阻塞的是整个界面）。
	virtual void OnOutboundRequest(const char* rpcId, const char* method, const char* argsJson) = 0;

	// ⚠️ 以后只许在末尾追加。
};
Q_DECLARE_INTERFACE(VirtualApiSink, "com.DSH_HUB.VirtualApiSink/1.0")
