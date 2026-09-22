#pragma once

// ------------------------------------------------------------------
// SessionCatalog.h
// ------------------------------------------------------------------
// 侧边栏“工作区 / 会话”列表的纯逻辑模型（不依赖任何 Qt Widget）：
//   - 解析 session.list 的 JSON 回包（工作区由 workspace/follow 的 baseline 提供）；
//   - 维护 sessionId -> 归属工作区、标题、归档状态；
//   - 派生视图需要的只读结果（按工作区分组、自动选中）。
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

	// 0.1.5：session/list 行里还带着两个有用的事实
	//   asOfSeq          projections.asOfSeq —— 该会话投影到的日志位置。
	//                    follow 快照迟迟不来时用它当 session/page 的 throughSeq 回落值。
	//   modelSelection*  该会话自己记录的模型选择（next 优先，退回 lastUsed）。
	//                    会话级的"当前模型"只在这里，catalog 接口给的是部署默认值。
	int asOfSeq = 0;
	bool hasModelSelection = false;
	QString modelProvider;
	QString modelId;
	QString reasoningEffort;
};

// session.list 一次回包的结构化结果（工作区不再随它一起回来）。
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

	/** 该会话投影到的日志位置（projections.asOfSeq）；没有该会话时返回 0。 */
	int asOfSeqFor(const QString& sessionId) const;

	/**
	 * 该会话自己记录的模型选择（projections.values.modelSelection.next，
	 * 退回 lastUsed）。返回 false 表示服务端还没记录过该会话的选择。
	 */
	bool modelSelectionFor(const QString& sessionId,
		QString* provider, QString* model, QString* reasoningEffort) const;

	const QVector<WorkspaceRecord>& workspaces() const;
	// 需要显示在侧边栏的会话（未归档、非子代理）
	QVector<SessionRecord> visibleSessions() const;

	// 自动选中：列表中第一个“非运行中、非子代理”的会话，无则返回空串
	QString autoSelectSessionId() const;

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
