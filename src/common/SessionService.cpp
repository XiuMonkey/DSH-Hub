#include "SessionService.h"

#include "DshApiClient.h"
#include "SessionCommands.h"
#include "SessionPrefetcher.h"
#include "TimingLogger.h"

#include <QDir>
#include <QFile>
#include <QJsonObject>

#include <memory>

namespace
{
	const char* const kSessionListMethod = "session.list";
	const char* const kWorkspaceListMethod = "workspace.list";
	const char* const kSessionCreateMethod = "session.create";

	// 每个可见会话预取的历史条数
	constexpr int kPrefetchMessageCount = 20;

	// 刷新过程中的累积状态：两个 RPC 都回来后统一应用
	struct RefreshState
	{
		SessionListSnapshot snapshot;
		bool workspacesDone = false;
		bool sessionsDone = false;
	};

	bool isComplete(const RefreshState& state)
	{
		return state.workspacesDone && state.sessionsDone;
	}
}

void SessionService::refreshSessions(
	DshApiClient* api,
	SessionPrefetcher* prefetcher,
	SessionCatalog* catalog,
	const std::function<void(const QString& autoSelectSessionId)>& onReady,
	const std::function<void(const DshApiClient::RpcError& error)>& onError)
{
	if (!api || !catalog)
		return;

	// workspace.list 与 session.list 互不依赖：并行发出，两个都返回后再统一
	// 应用（保证 archived/workspace 状态先就位，再处理会话列表与自动选中），
	// 省掉原先一次串行 RPC 往返。
	auto state = std::make_shared<RefreshState>();

	auto applyWhenBothDone = [api, prefetcher, catalog, state, onReady, onError]() {
		if (!isComplete(*state))
			return;

		catalog->applySnapshot(state->snapshot);

		if (!state->snapshot.sessionsOk) {
			if (onError)
				onError(DshApiClient::RpcError{ state->snapshot.errorCode, state->snapshot.errorMessage, {} });
			return;
		}

		// 先确定会被自动选中的会话（第一个非 running 会话）：
		// 它马上要由 HistoryLoader 拉全量历史，预取同一份历史只会
		// 让服务端多建一次视图、客户端多一次重复渲染，直接跳过。
		const QString autoSelectId = catalog->autoSelectSessionId();

		if (prefetcher) {
			for (const QString& sid : catalog->prefetchSessionIds(autoSelectId))
				prefetcher->prefetchHistory(api->baseUrl(), sid, kPrefetchMessageCount);
		}

		TimingLogger::mark(QStringLiteral("session.list loaded (%1 items) + prefetch scheduled")
			.arg(state->snapshot.sessions.size()));

		if (onReady)
			onReady(autoSelectId);
		};

	api->callMethod(
		QLatin1String(kWorkspaceListMethod),
		{},
		[state, applyWhenBothDone](const QJsonObject& value) {
			state->snapshot.workspacesOk = true;
			state->snapshot.archivedSessionIds = SessionCatalog::parseArchivedSessionIds(value);
			state->snapshot.workspaces = value.value(QStringLiteral("items")).toArray();
			state->workspacesDone = true;
			applyWhenBothDone();
		},
		[state, applyWhenBothDone](const DshApiClient::RpcError&) {
			state->workspacesDone = true;
			applyWhenBothDone();
		});

	api->callMethod(
		QLatin1String(kSessionListMethod),
		{},
		[state, applyWhenBothDone](const QJsonObject& value) {
			state->snapshot.sessions = value.value(QStringLiteral("items")).toArray();
			state->snapshot.sessionsOk = true;
			state->sessionsDone = true;
			applyWhenBothDone();
		},
		[state, applyWhenBothDone](const DshApiClient::RpcError& error) {
			state->snapshot.errorCode = error.code;
			state->snapshot.errorMessage = error.message;
			state->sessionsDone = true;
			applyWhenBothDone();
		});
}

void SessionService::createSession(
	DshApiClient* api,
	const QString& workspaceId,
	const std::function<void(const QString& sessionId)>& onCreated,
	const std::function<void(const DshApiClient::RpcError& error)>& onError)
{
	if (!api)
		return;

	api->callMethod(
		QLatin1String(kSessionCreateMethod),
		SessionCommands::sessionCreate(workspaceId),
		[onCreated](const QJsonObject& value) {
			const QString sid = value.value(QStringLiteral("sessionId")).toString();
			if (sid.isEmpty())
				return;
			if (onCreated)
				onCreated(sid);
		},
		[onError](const DshApiClient::RpcError& error) {
			if (onError)
				onError(error);
		});
}

void SessionService::refreshTitles(
	DshApiClient* api,
	const std::function<void(const QString& sessionId, const QString& title)>& onTitle)
{
	if (!api)
		return;

	api->callMethod(
		QLatin1String(kSessionListMethod),
		{},
		[onTitle](const QJsonObject& value) {
			const QJsonArray items = value.value(QStringLiteral("items")).toArray();
			for (const auto& item : items) {
				const QJsonObject session = item.toObject();
				const QString sid = session.value(QStringLiteral("sessionId")).toString();
				if (sid.isEmpty())
					continue;

				const QString label = SessionCatalog::projectionTitle(session);
				if (label.isEmpty())
					continue;

				if (onTitle)
					onTitle(sid, label);
			}
		},
		[](const DshApiClient::RpcError&) {});
}

void SessionService::clearAllSessionData(const QString& dshHome)
{
	if (dshHome.isEmpty())
		return;

	QDir sessionsDir(dshHome + QStringLiteral("/sessions"));
	if (sessionsDir.exists()) {
		sessionsDir.removeRecursively();
		sessionsDir.mkpath(QStringLiteral("."));
	}

	// 工作区清单保存在 storage domain 中，不删除的话重启后会重新出现旧工作区
	QFile::remove(dshHome + QStringLiteral("/storages/workspace.json"));
}