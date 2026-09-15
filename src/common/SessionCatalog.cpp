		#include "SessionCatalog.h"

#include <QCoreApplication>

// ------------------------------------------------------------------
// SessionCatalog.cpp
// ------------------------------------------------------------------
// 会话/工作区列表的解析与派生逻辑。所有函数都不触碰控件，
// 因此可以脱离 UI 单独做单元测试。
// ------------------------------------------------------------------

namespace
{
	const char* const kWorkspaceIdKey = "workspaceId";
	const char* const kTitleKey = "title";
	const char* const kSessionIdsKey = "sessionIds";
}

void SessionCatalog::clear()
{
	m_workspaces.clear();
	m_sessions.clear();
	m_archivedSessionIds.clear();
}

// ------------------------------------------------------------------
// 写入
// ------------------------------------------------------------------

void SessionCatalog::applySnapshot(const SessionListSnapshot& snapshot)
{
	// 0.1.5：工作区清单与归档集合来自 mux 上的 workspace/follow，不再随会话列表回来。
	// 因此 workspacesOk=false 表示"本次没有任何工作区信息"，这时必须**保留**现有分组，
	// 而不是清空——清空会把 workspace/follow 的基线抹掉。
	if (snapshot.workspacesOk) {
		setArchivedSessionIds(snapshot.archivedSessionIds);
		setWorkspaces(snapshot.workspaces);
	}

	if (snapshot.sessionsOk)
		setSessions(snapshot.sessions);
}

void SessionCatalog::setWorkspaces(const QJsonArray& items)
{
	m_workspaces.clear();

	for (const auto& item : items) {
		const QJsonObject obj = item.toObject();
		const QString workspaceId = obj.value(QLatin1String(kWorkspaceIdKey)).toString();
		if (workspaceId.isEmpty())
			continue;

		WorkspaceRecord record;
		record.workspaceId = workspaceId;
		const QString title = obj.value(QLatin1String(kTitleKey)).toString();
		record.title = title.isEmpty() ? workspaceId : title;

		const QJsonArray sessionIds = obj.value(QLatin1String(kSessionIdsKey)).toArray();
		for (const auto& sidValue : sessionIds) {
			const QString sid = sidValue.toString();
			if (!sid.isEmpty())
				record.sessionIds.append(sid);
		}

		m_workspaces.append(record);
	}
}

void SessionCatalog::setSessions(const QJsonArray& items)
{
	m_sessions.clear();

	for (const auto& item : items) {
		const QJsonObject session = item.toObject();
		if (isSubagent(session))
			continue;

		const QString sid = session.value(QStringLiteral("sessionId")).toString();
		if (sid.isEmpty())
			continue;

		SessionRecord record;
		record.sessionId = sid;
		record.title = sessionTitle(session);
		record.workspaceId = workspaceFor(sid);
		record.running = session.value(QStringLiteral("running")).toBool();
		record.archived = m_archivedSessionIds.contains(sid);

		// 0.1.5：把两个会话级事实一起带进来（游标回落值 + 该会话的模型选择）
		const QJsonObject projections = session.value(QStringLiteral("projections")).toObject();
		record.asOfSeq = projections.value(QStringLiteral("asOfSeq")).toInt();

		const QJsonObject values = projections.value(QStringLiteral("values")).toObject();
		const QJsonObject selection = values.value(QStringLiteral("modelSelection")).toObject();
		// next = 用户为下一轮选定的；没有再退回 lastUsed
		QJsonObject chosen = selection.value(QStringLiteral("next")).toObject();
		if (chosen.isEmpty())
			chosen = selection.value(QStringLiteral("lastUsed")).toObject();
		if (!chosen.isEmpty()) {
			record.hasModelSelection = true;
			record.modelProvider = chosen.value(QStringLiteral("provider")).toString();
			record.modelId = chosen.value(QStringLiteral("model")).toString();
			record.reasoningEffort = chosen.value(QStringLiteral("reasoningEffort")).toString();
		}

		m_sessions.append(record);
	}
}

void SessionCatalog::setArchivedSessionIds(const QSet<QString>& ids)
{
	m_archivedSessionIds = ids;

	for (SessionRecord& record : m_sessions)
		record.archived = m_archivedSessionIds.contains(record.sessionId);
}

bool SessionCatalog::addSession(const QString& sessionId,
	const QString& title,
	const QString& workspaceId)
{
	if (sessionId.isEmpty() || m_archivedSessionIds.contains(sessionId))
		return false;

	if (SessionRecord* existing = find(sessionId)) {
		existing->title = title;
		if (!workspaceId.isEmpty())
			existing->workspaceId = workspaceId;
		return true;
	}

	SessionRecord record;
	record.sessionId = sessionId;
	record.title = title;
	record.workspaceId = workspaceId;
	m_sessions.append(record);
	return true;
}

bool SessionCatalog::updateTitle(const QString& sessionId, const QString& title)
{
	SessionRecord* record = find(sessionId);
	if (!record)
		return false;

	record->title = title;
	return true;
}

// ------------------------------------------------------------------
// 查询
// ------------------------------------------------------------------

QString SessionCatalog::titleFor(const QString& sessionId) const
{
	for (const SessionRecord& record : m_sessions) {
		if (record.sessionId == sessionId)
			return record.title.isEmpty() ? record.sessionId : record.title;
	}
	return QString();
}

int SessionCatalog::asOfSeqFor(const QString& sessionId) const
{
	if (const SessionRecord* record = find(sessionId))
		return record->asOfSeq;
	return 0;
}

bool SessionCatalog::modelSelectionFor(const QString& sessionId,
	QString* provider, QString* model, QString* reasoningEffort) const
{
	const SessionRecord* record = find(sessionId);
	if (!record || !record->hasModelSelection)
		return false;

	if (provider)
		*provider = record->modelProvider;
	if (model)
		*model = record->modelId;
	if (reasoningEffort)
		*reasoningEffort = record->reasoningEffort;
	return true;
}

QString SessionCatalog::workspaceFor(const QString& sessionId) const
{
	// 会话自身记录的归属优先：addSession / addSessionToWorkspace 会显式写入，
	// 否则界面在“在工作区里新建会话”之后会把按钮挂到未分组下面。
	if (const SessionRecord* record = find(sessionId)) {
		if (!record->workspaceId.isEmpty())
			return record->workspaceId;
	}

	// 其次看工作区记录（workspace/follow 的 baseline）给出的 sessionIds 归属
	for (const WorkspaceRecord& workspace : m_workspaces) {
		if (workspace.sessionIds.contains(sessionId))
			return workspace.workspaceId;
	}
	return QString();
}

const QVector<WorkspaceRecord>& SessionCatalog::workspaces() const
{
	return m_workspaces;
}

QVector<SessionRecord> SessionCatalog::visibleSessions() const
{
	QVector<SessionRecord> visible;
	visible.reserve(m_sessions.size());
	for (const SessionRecord& record : m_sessions) {
		if (!record.archived)
			visible.append(record);
	}
	return visible;
}

QString SessionCatalog::autoSelectSessionId() const
{
	for (const SessionRecord& record : m_sessions) {
		if (!record.running)
			return record.sessionId;
	}
	return QString();
}

// ------------------------------------------------------------------
// JSON 解析辅助
// ------------------------------------------------------------------

bool SessionCatalog::isSubagent(const QJsonObject& session)
{
	const QString origin = session.value(QStringLiteral("origin")).toString();
	return origin == QStringLiteral("subagent")
		|| session.contains(QStringLiteral("parentSessionId"));
}

QString SessionCatalog::projectionTitle(const QJsonObject& session)
{
	const QJsonObject projections = session.value(QStringLiteral("projections")).toObject();
	const QJsonObject values = projections.value(QStringLiteral("values")).toObject();

	QString label = values.value(QStringLiteral("title")).toString();
	if (label.isEmpty())
		label = values.value(QStringLiteral("sessionTitle")).toString();
	if (label.isEmpty())
		label = values.value(QStringLiteral("session.title")).toString();
	return label;
}

QString SessionCatalog::sessionTitle(const QJsonObject& session)
{
	const QString label = projectionTitle(session);
	return label.isEmpty() ? QCoreApplication::translate("SessionCatalog", "未命名会话") : label;
}

QSet<QString> SessionCatalog::parseArchivedSessionIds(const QJsonObject& workspaceListValue)
{
	QSet<QString> ids;
	const QJsonArray archived = workspaceListValue.value(QStringLiteral("archivedSessionIds")).toArray();
	for (const auto& value : archived) {
		const QString id = value.toString();
		if (!id.isEmpty())
			ids.insert(id);
	}
	return ids;
}

SessionRecord* SessionCatalog::find(const QString& sessionId)
{
	for (SessionRecord& record : m_sessions) {
		if (record.sessionId == sessionId)
			return &record;
	}
	return nullptr;
}

const SessionRecord* SessionCatalog::find(const QString& sessionId) const
{
	for (const SessionRecord& record : m_sessions) {
		if (record.sessionId == sessionId)
			return &record;
	}
	return nullptr;
}