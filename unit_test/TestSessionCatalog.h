#pragma once

// ------------------------------------------------------------------
// TestSessionCatalog.h
// ------------------------------------------------------------------
// SessionCatalog（从 Sidebar 抽出的会话/工作区解析与派生逻辑）单元测试。
// 覆盖：子代理过滤、标题回退、归档过滤、工作区归属、
//       自动选中会话与预取目标列表。
// ------------------------------------------------------------------

#include <QObject>

class TestSessionCatalog : public QObject
{
	Q_OBJECT

private slots:
	// JSON 解析
	void workspacesParsedAndEmptyIdSkipped();
	void workspaceTitleFallsBackToId();
	void sessionsParsedWithProjectionTitle();
	void sessionsWithoutTitleUsePlaceholder();
	void subagentSessionsAreSkipped();
	void archivedSessionIdsParsed();

	// 派生结果
	void archivedSessionsHiddenButStillAutoSelectable();
	void autoSelectSkipsRunningSessions();
	void autoSelectReturnsEmptyWhenAllRunning();
	void addSessionIgnoresArchived();
	void addSessionAssignsWorkspace();
	void updateTitleOnlyForKnownSession();
	void applySnapshotKeepsArchivedWhenWorkspaceListFails();
	void applySnapshotSkipsSessionsWhenSessionListFails();
};
