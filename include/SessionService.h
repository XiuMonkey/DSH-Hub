#pragma once

// ------------------------------------------------------------------
// SessionService.h
// ------------------------------------------------------------------
// 会话相关的“功能逻辑”统一入口（不依赖任何 Qt Widget）：
//   - 并行拉取 workspace.list / session.list 并解析进 SessionCatalog；
//   - 调度历史预取（跳过马上要被全量加载的那个会话）；
//   - session.create、session.list 标题回填；
//   - 清空 DSH home 下的会话数据。
//
// UI 层（Sidebar）只保留“结果回来后怎么改界面”，不再手搓 RPC 与 JSON。
// 回调都在主线程触发（DshApiClient 的回调本来就在主线程）。
// ------------------------------------------------------------------

#include "SessionCatalog.h"
// 回调签名里用到 DshApiClient::RpcError（嵌套类型），因此需要完整定义
#include "DshApiClient.h"

#include <QString>
#include <functional>

class SessionPrefetcher;

class SessionService
{
public:
	// 并行拉取 workspace.list + session.list，两个都返回后：
	//   1) 把结果写入 catalog（工作区/会话/归档状态）；
	//   2) 为除自动选中会话外的可见会话调度历史预取；
	//   3) 回调 onReady(autoSelectSessionId) 或 onError(error)。
	// workspace.list 失败不算致命（按“无工作区”处理），
	// session.list 失败才走 onError，且此时不会写入会话列表。
	static void refreshSessions(
		DshApiClient* api,
		SessionPrefetcher* prefetcher,
		SessionCatalog* catalog,
		const std::function<void(const QString& autoSelectSessionId)>& onReady,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);

	// 新建会话；成功后回调新建出的 sessionId
	static void createSession(
		DshApiClient* api,
		const QString& workspaceId,
		const std::function<void(const QString& sessionId)>& onCreated,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);

	// 重新拉取 session.list，把每个会话的最新标题逐条回调给 UI
	static void refreshTitles(
		DshApiClient* api,
		const std::function<void(const QString& sessionId, const QString& title)>& onTitle);

	// 清空 DSH home 下的会话数据：sessions 目录 + storage domain 里的工作区清单
	static void clearAllSessionData(const QString& dshHome);
};
