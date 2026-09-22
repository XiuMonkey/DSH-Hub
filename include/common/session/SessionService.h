#pragma once

// 会话相关“功能逻辑”的统一入口（无 Qt Widget 依赖）：拉取并解析 session.list、触发侧栏重建与自动选中、session.create、标题回填、清空 DSH home 会话数据。
// 回调都在主线程触发（DshApiClient 的回调本来就在主线程）。

#include "common/session/SessionCatalog.h"
// 回调签名用到 DshApiClient::RpcError（嵌套类型），因此需要完整定义
#include "network/DshApiClient.h"

#include <QString>
#include <functional>

class SessionService
{
public:
	// 拉取 session.list 并写入 catalog（不含工作区，后者由 workspace/follow 提供）；成功回调 onReady(autoSelectSessionId)，失败回调 onError 且此时不写会话列表。
	static void refreshSessions(
		DshApiClient* api,
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
