#pragma once

// 侧边栏“工作区 / 会话”列表的纯逻辑模型（无 Qt Widget 依赖）：解析 session.list 回包，维护 sessionId 的归属/标题/归档，并派生分组与自动选中结果。
// 工作区不随 session.list 返回，只由 workspace/follow 的 baseline 提供。

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

struct SessionRecord
{
	QString sessionId;
	QString title;
	QString workspaceId;
	bool running = false;
	bool archived = false;

	// asOfSeq = projections.asOfSeq（该会话投影到的日志位置），follow 快照迟迟不来时当 session/page 的 throughSeq 回落值。
	int asOfSeq = 0;
	// 下面三个 + hasModelSelection = 会话自己记录的模型选择（next 优先，退回 lastUsed）；会话级“当前模型”只在这里，catalog 给的是部署默认值。
	bool hasModelSelection = false;
	QString modelProvider;
	QString modelId;
	QString reasoningEffort;
};

// session.list 一次回包的结构化结果（工作区不随之返回），避免把 JSON 细节带进 UI。
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

	// 按“先工作区、后会话”的顺序应用一次回包
	void applySnapshot(const SessionListSnapshot& snapshot);
	void setWorkspaces(const QJsonArray& items);
	void setSessions(const QJsonArray& items);
	void setArchivedSessionIds(const QSet<QString>& ids);

	// 新增/更新单个会话；返回 false = 被忽略（sessionId 为空或已归档），调用方不应再渲染它。
	bool addSession(const QString& sessionId,
		const QString& title,
		const QString& workspaceId = QString());
	// 更新已存在会话的标题；会话不存在时返回 false
	bool updateTitle(const QString& sessionId, const QString& title);

	QString titleFor(const QString& sessionId) const;
	QString workspaceFor(const QString& sessionId) const;

	// 该会话投影到的日志位置（projections.asOfSeq）；没有该会话时返回 0。
	int asOfSeqFor(const QString& sessionId) const;

	// 该会话自己记录的模型选择（modelSelection.next → lastUsed）；返回 false = 服务端还没记录过该会话的选择。
	bool modelSelectionFor(const QString& sessionId,
		QString* provider, QString* model, QString* reasoningEffort) const;

	const QVector<WorkspaceRecord>& workspaces() const;
	// 需要显示在侧边栏的会话（未归档、非子代理）
	QVector<SessionRecord> visibleSessions() const;

	// 自动选中：第一个“非运行中、非子代理”的会话，无则返回空串
	QString autoSelectSessionId() const;

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
