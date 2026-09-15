#include "SessionService.h"

#include "DshApiClient.h"
#include "SessionCommands.h"
#include "TimingLogger.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonObject>

#include <memory>

namespace
{
	// dsh 0.1.5：endpoint 用斜杠；session/list 的 wire 名是 _request
	const char* const kSessionListMethod = "session/list";
	const char* const kSessionCreateMethod = "session/create";
}

void SessionService::refreshSessions(
	DshApiClient* api,
	SessionCatalog* catalog,
	const std::function<void(const QString& autoSelectSessionId)>& onReady,
	const std::function<void(const DshApiClient::RpcError& error)>& onError)
{
	if (!api || !catalog)
		return;

	auto state = std::make_shared<SessionListSnapshot>();

	auto applySnapshot = [api, catalog, state, onReady, onError]() {
		catalog->applySnapshot(*state);

		if (!state->sessionsOk) {
			if (onError)
				onError(DshApiClient::RpcError{ state->errorCode, state->errorMessage, {} });
			return;
		}

		const QString autoSelectId = catalog->autoSelectSessionId();

		TimingLogger::mark(QStringLiteral("session/list loaded (%1 items)")
			.arg(state->sessions.size()));

		if (onReady)
			onReady(autoSelectId);
		};

	api->callMethod(
		QLatin1String(kSessionListMethod),
		SessionCommands::sessionList(),
		[state, applySnapshot](const QJsonObject& value) {
			const QJsonArray items = value.value(QStringLiteral("items")).toArray();
			state->sessions = items;
			state->sessionsOk = true;
			// 0.1.5：session/list 里没有工作区信息（workspace.list 已删除），
			// workspacesOk 保持 false 让 catalog 保留 workspace/follow 给的基线分组。
			state->workspacesOk = false;
			applySnapshot();
		},
		[state, applySnapshot](const DshApiClient::RpcError& error) {
			state->errorCode = error.code;
			state->errorMessage = error.message;
			state->sessionsOk = false;
			applySnapshot();
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
		SessionCommands::sessionList(),
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