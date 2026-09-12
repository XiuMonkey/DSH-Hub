#pragma once

// ------------------------------------------------------------------
// SessionCatalog.h
// ------------------------------------------------------------------
// 侧边栏“工作区 / 会话”列表的纯逻辑模型（不依赖任何 Qt Widget）：
//   - 解析 workspace.list / session.list 的 JSON 回包；
//   - 维护 sessionId -> 归属工作区、标题、归档状态；
//   - 派生视图需要的只读结果（按工作区分组、自动选中、预取目标）。
//
// UI 层（Sidebar / WorkspaceList）只负责把这里的数据画成按钮，
// 不再自己解析 JSON、也不再自己维护 sessionId -> workspaceId 映射。
// ------------------------------------------------------------------

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

// 工作区记录（workspaceId 为空表示“未分组”）
struct WorkspaceRecord
{
	QString workspaceId;
	QString title;
	QStringList sessionIds;
};

// 会话记录
struct SessionRecord
{
	QString sessionId;
	QString title;
	QString workspaceId;
	bool running = false;
	bool archived = false;
};

// workspace.list + session.list 一次完整回包的结构化结果。
// 供 SessionService 填充、SessionCatalog 消费，避免把 JSON 细节带进 UI。
struct SessionListSnapshot
{
	bool workspacesOk = false;
	QSet<QString> archivedSessionIds;
	QJsonArray workspaces;

	bool sessionsOk = false;
	QJsonArray sessions;

	QString errorCode;
	QString errorMessage;
};

class SessionCatalog
{
public:
	void clear();

	// ---- 写入 ----
	// 按“先工作区、后会话”的顺序应用一次回包（与刷新流程的语义一致）
	void applySnapshot(const SessionListSnapshot& snapshot);
	void setWorkspaces(const QJsonArray& items);
	void setSessions(const QJsonArray& items);
	void setArchivedSessionIds(const QSet<QString>& ids);

	// 新增/更新单个会话。
	// 返回 false 表示该会话被忽略（sessionId 为空或已归档），调用方不应再渲染它。
	bool addSession(const QString& sessionId,
		const QString& title,
		const QString& workspaceId = QString());
	// 更新已存在会话的标题；会话不存在时返回 false
	bool updateTitle(const QString& sessionId, const QString& title);

	// ---- 查询 ----
	QString titleFor(const QString& sessionId) const;
	QString workspaceFor(const QString& sessionId) const;

	const QVector<WorkspaceRecord>& workspaces() const;
	// 需要显示在侧边栏的会话（未归档、非子代理）
	QVector<SessionRecord> visibleSessions() const;

	// 自动选中：列表中第一个“非运行中、非子代理”的会话，无则返回空串
	QString autoSelectSessionId() const;
	// 预取目标：除 excludeSessionId 外的全部会话（子代理除外）
	QStringList prefetchSessionIds(const QString& excludeSessionId) const;

	// ---- JSON 解析辅助（纯函数） ----
	static bool isSubagent(const QJsonObject& session);
	// 从 projections.values 取标题（取不到返回空串）
	static QString projectionTitle(const QJsonObject& session);
	// 展示用标题（取不到时回退到“未命名会话”）
	static QString sessionTitle(const QJsonObject& session);
	static QSet<QString> parseArchivedSessionIds(const QJsonObject& workspaceListValue);

private:
	SessionRecord* find(const QString& sessionId);
	const SessionRecord* find(const QString& sessionId) const;

	QVector<WorkspaceRecord> m_workspaces;
	QVector<SessionRecord> m_sessions;
	QSet<QString> m_archivedSessionIds;
};
