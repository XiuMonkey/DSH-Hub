#pragma once

// 后端接管（API takeover）的两个公共接口：宿主实现接口一（挂在 DshApiClient 上），客户端扩展
// 实现接口二（挂在插件根对象上）。接管时内置 DSH 进程停掉，全部出站交给扩展，宿主 UI 不改；
// 入站数据注入见 VirtualMain（VirtualCommon.h）。设计文档：misc/DESIGN_NOTES.zh-CN.md、API_TAKEOVER_PLAN.zh-CN.md §2.6。
//
// ABI 四条：全内联（out-of-line 成员 = 真外部符号，插件 LNK2019）；虚方法只在末尾追加（vtable
// 槽位 = 声明顺序）；改 IID = 改 ABI（旧插件静默拿 nullptr）；参数可带 Qt 类型。不把虚方法加进
// DshApiClient.h 的原因：那个类方法全在 .cpp（out-of-line），插件一碰类型就 LNK2019。
//
// 通讯形态：插件 → 宿主走接口一（findObject(kApiClient) 后 qobject_cast）；宿主 → 插件走接口二
// （装载时 cast 一次登记到 kApiSink）；入站注入走 VirtualMain（findObject(kMainWindow)）。
// 出站一元 RPC 汇成 (method, argsJson)，扩展用 CompleteCall / FailCall 回填成功回调该拿的 value
// （不是 server-response 信封）；流控制只有 "$takeover/stream-open|cancel" 两条 fire-and-forget。
// 喂数据时机是 Takenover(true)：接管后 baseUrl 为空，那 8 个出站方法没有"已就绪"回执；
// 回填不认识的 rpcId 只记一条日志。错误码：宿主上层只拼文本不比较 code，扩展自定即可；
// 宿主自产码统一带 takenover- 前缀（no-sink / timeout / released / bad-result）。

#include <qplugin.h>

// 接口一：宿主实现（挂在 DshApiClient 上），插件调用 —— 插件 → 宿主。
class VirtualApiHost
{
public:
	virtual ~VirtualApiHost() = default;

	// 拨动"后端已被接管"开关：true = 宿主不再走 HTTP/WebSocket、出站全交扩展；false = 交还
	// （不会自动重启内置服务端，恢复只能重启客户端或触发 ServerManager::restart()）。宿主动作幂等；
	// 切主题重建主窗口会对同一插件再调 attachHost()，插件应重新取接口并再调一次 Takenover(true)。
	virtual void Takenover(bool on) = 0;

	// 回填一次出站请求：resultJson 是成功回调该拿到的那份 value（不是 server-response 信封），
	// 可以是对象、数组或裸标量。rpcId 不认识（重复回填 / fire-and-forget 的 id）只记 qWarning。
	virtual void CompleteCall(const char* rpcId, const char* resultJson) = 0;

	// 回填失败：code / message 原样进 RpcError（上层只拼文本、不比较 code）；code 为空时宿主
	// 用 takenover-error 兜底。调用方没提供 onError 时只记日志。
	virtual void FailCall(const char* rpcId, const char* code, const char* message) = 0;

	// 接管态自检口：Takenover(bool) 无回执，调完读它确认有没有被接受；只报状态不区分接管者
	virtual bool IsTakenover() const = 0;

	// 以后只许在末尾追加。
};
Q_DECLARE_INTERFACE(VirtualApiHost, "com.DSH_HUB.VirtualApiHost/1.0")

// 接口二：插件实现（挂在插件的根对象上，与 DshHostPlugin 同一个对象），宿主调用 —— 宿主 → 插件。
// 插件类里必须写 `Q_INTERFACES(DshHostPlugin VirtualApiSink)`（多个 IID 空格分隔）：宿主的
// qobject_cast 走 moc 生成的 qt_metacast，它只认 Q_INTERFACES 列出的 IID。漏了不报错，只是
// 永远 cast 出 nullptr，然后宿主在装载期就判定"这个扩展不能接管"。
class VirtualApiSink
{
public:
	virtual ~VirtualApiSink() = default;

	// 宿主把一次出站请求交给扩展（形态见文件头）。回调是 std::function 不是 metatype，留在宿主
	// m_pending 表里、等回填后由宿主触发；三个 const char* 只在本次调用有效，要自己拷贝。
	// GUI 线程同步调用，别做耗时的事（阻塞的是整个界面）。
	virtual void OnOutboundRequest(const char* rpcId, const char* method, const char* argsJson) = 0;

	// 以后只许在末尾追加。
};
Q_DECLARE_INTERFACE(VirtualApiSink, "com.DSH_HUB.VirtualApiSink/1.0")
