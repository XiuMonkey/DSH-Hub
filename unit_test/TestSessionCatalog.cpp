// ------------------------------------------------------------------
// TestSessionCatalog.cpp
// ------------------------------------------------------------------
// 见 TestSessionCatalog.h。
// ------------------------------------------------------------------

#include "TestSessionCatalog.h"

#include "SessionCatalog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

namespace
{
	QJsonArray parseArray(const char* json)
	{
		return QJsonDocument::fromJson(json).array();
	}

	QJsonObject parseObject(const char* json)
	{
		return QJsonDocument::fromJson(json).object();
	}

	// 构造一个带 projections.values.title 的会话条目
	QJsonObject session(const QString& id, const QString& title, bool running = false)
	{
		QJsonObject values;
		if (!title.isEmpty())
			values.insert(QStringLiteral("title"), title);

		QJsonObject projections;
		projections.insert(QStringLiteral("values"), values);

		QJsonObject object;
		object.insert(QStringLiteral("sessionId"), id);
		object.insert(QStringLiteral("running"), running);
		object.insert(QStringLiteral("projections"), projections);
		return object;
	}
}

void TestSessionCatalog::workspacesParsedAndEmptyIdSkipped()
{
	SessionCatalog catalog;
	catalog.setWorkspaces(parseArray(R"([
		{ "workspaceId": "w1", "title": "工作区一", "sessionIds": ["s1", "s2"] },
		{ "workspaceId": "", "title": "无 id", "sessionIds": ["s3"] },
		{ "workspaceId": "w2", "title": "工作区二", "sessionIds": ["s4", ""] }
	])"));

	QCOMPARE(catalog.workspaces().size(), 2);
	QCOMPARE(catalog.workspaces().at(0).workspaceId, QStringLiteral("w1"));
	QCOMPARE(catalog.workspaces().at(0).title, QStringLiteral("工作区一"));
	QCOMPARE(catalog.workspaces().at(0).sessionIds, QStringList({ QStringLiteral("s1"), QStringLiteral("s2") }));
	// 空 id 的会话归属不会进入列表
	QCOMPARE(catalog.workspaces().at(1).sessionIds, QStringList({ QStringLiteral("s4") }));
}

void TestSessionCatalog::workspaceTitleFallsBackToId()
{
	SessionCatalog catalog;
	catalog.setWorkspaces(parseArray(R"([{ "workspaceId": "w1" }])"));

	QCOMPARE(catalog.workspaces().size(), 1);
	QCOMPARE(catalog.workspaces().at(0).title, QStringLiteral("w1"));
}

void TestSessionCatalog::sessionsParsedWithProjectionTitle()
{
	SessionCatalog catalog;
	catalog.setWorkspaces(parseArray(R"([{ "workspaceId": "w1", "sessionIds": ["s1"] }])"));

	QJsonArray items;
	items.append(session(QStringLiteral("s1"), QStringLiteral("标题一")));
	catalog.setSessions(items);

	QCOMPARE(catalog.visibleSessions().size(), 1);
	QCOMPARE(catalog.visibleSessions().at(0).sessionId, QStringLiteral("s1"));
	QCOMPARE(catalog.visibleSessions().at(0).title, QStringLiteral("标题一"));
	// 归属来自 workspace.list 的 sessionIds
	QCOMPARE(catalog.visibleSessions().at(0).workspaceId, QStringLiteral("w1"));
	QCOMPARE(catalog.titleFor(QStringLiteral("s1")), QStringLiteral("标题一"));
	QCOMPARE(catalog.workspaceFor(QStringLiteral("s1")), QStringLiteral("w1"));
}

void TestSessionCatalog::sessionsWithoutTitleUsePlaceholder()
{
	SessionCatalog catalog;

	QJsonArray items;
	items.append(session(QStringLiteral("s1"), QString()));
	// session.title 作为第二优先级
	QJsonObject values;
	values.insert(QStringLiteral("session.title"), QStringLiteral("紧凑标题"));
	QJsonObject projections;
	projections.insert(QStringLiteral("values"), values);
	QJsonObject alt;
	alt.insert(QStringLiteral("sessionId"), QStringLiteral("s2"));
	alt.insert(QStringLiteral("projections"), projections);
	items.append(alt);

	catalog.setSessions(items);

	QCOMPARE(catalog.titleFor(QStringLiteral("s1")), QStringLiteral("未命名会话"));
	QCOMPARE(catalog.titleFor(QStringLiteral("s2")), QStringLiteral("紧凑标题"));
}

void TestSessionCatalog::subagentSessionsAreSkipped()
{
	SessionCatalog catalog;

	QJsonObject byOrigin = session(QStringLiteral("sub1"), QStringLiteral("子代理"));
	byOrigin.insert(QStringLiteral("origin"), QStringLiteral("subagent"));

	QJsonObject byParent = session(QStringLiteral("sub2"), QStringLiteral("子代理2"));
	byParent.insert(QStringLiteral("parentSessionId"), QStringLiteral("s1"));

	QJsonArray items;
	items.append(byOrigin);
	items.append(byParent);
	items.append(session(QStringLiteral("s1"), QStringLiteral("主会话")));

	catalog.setSessions(items);

	QCOMPARE(catalog.visibleSessions().size(), 1);
	QCOMPARE(catalog.visibleSessions().at(0).sessionId, QStringLiteral("s1"));
	// 子代理会话既不显示、也不在目录里（titleFor 对未知会话返回空串）
	QVERIFY(catalog.titleFor(QStringLiteral("sub1")).isEmpty());
	QVERIFY(catalog.titleFor(QStringLiteral("sub2")).isEmpty());
}

void TestSessionCatalog::archivedSessionIdsParsed()
{
	const QJsonObject value = parseObject(R"({
		"archivedSessionIds": ["a1", "", "a2"]
	})");

	const QSet<QString> ids = SessionCatalog::parseArchivedSessionIds(value);
	QCOMPARE(ids.size(), 2);
	QVERIFY(ids.contains(QStringLiteral("a1")));
	QVERIFY(ids.contains(QStringLiteral("a2")));
}

void TestSessionCatalog::archivedSessionsHiddenButStillAutoSelectable()
{
	SessionCatalog catalog;
	catalog.setArchivedSessionIds({ QStringLiteral("s1") });

	QJsonArray items;
	items.append(session(QStringLiteral("s1"), QStringLiteral("已归档")));
	items.append(session(QStringLiteral("s2"), QStringLiteral("正常")));
	catalog.setSessions(items);

	// 已归档的不显示在侧边栏
	QCOMPARE(catalog.visibleSessions().size(), 1);
	QCOMPARE(catalog.visibleSessions().at(0).sessionId, QStringLiteral("s2"));

	// 但“第一个非 running 会话”的判定仍然按原始列表顺序（与原实现一致）
	QCOMPARE(catalog.autoSelectSessionId(), QStringLiteral("s1"));
}

void TestSessionCatalog::autoSelectSkipsRunningSessions()
{
	SessionCatalog catalog;

	QJsonArray items;
	items.append(session(QStringLiteral("s1"), QStringLiteral("运行中"), true));
	items.append(session(QStringLiteral("s2"), QStringLiteral("空闲")));
	items.append(session(QStringLiteral("s3"), QStringLiteral("空闲2")));
	catalog.setSessions(items);

	QCOMPARE(catalog.autoSelectSessionId(), QStringLiteral("s2"));
}

void TestSessionCatalog::autoSelectReturnsEmptyWhenAllRunning()
{
	SessionCatalog catalog;

	QJsonArray items;
	items.append(session(QStringLiteral("s1"), QStringLiteral("运行中"), true));
	catalog.setSessions(items);

	QVERIFY(catalog.autoSelectSessionId().isEmpty());
}

void TestSessionCatalog::addSessionIgnoresArchived()
{
	SessionCatalog catalog;
	catalog.setArchivedSessionIds({ QStringLiteral("gone") });

	// 返回值用于告诉 UI“不要为这个会话创建按钮”
	QVERIFY(!catalog.addSession(QStringLiteral("gone"), QStringLiteral("已删除")));
	QVERIFY(!catalog.addSession(QString(), QStringLiteral("空 id")));
	QVERIFY(catalog.addSession(QStringLiteral("s1"), QStringLiteral("新会话")));

	// 已归档的会话被忽略：既没进目录（titleFor 返回空串），也没有标题
	QVERIFY(catalog.titleFor(QStringLiteral("gone")).isEmpty());
	QCOMPARE(catalog.titleFor(QStringLiteral("s1")), QStringLiteral("新会话"));

	// 重复添加只更新标题，不产生重复记录
	QVERIFY(catalog.addSession(QStringLiteral("s1"), QStringLiteral("改名")));
	QCOMPARE(catalog.visibleSessions().size(), 1);
	QCOMPARE(catalog.titleFor(QStringLiteral("s1")), QStringLiteral("改名"));
}

void TestSessionCatalog::addSessionAssignsWorkspace()
{
	SessionCatalog catalog;
	catalog.setWorkspaces(parseArray(R"([{ "workspaceId": "w1", "title": "工作区一" }])"));

	QVERIFY(catalog.addSession(QStringLiteral("s1"), QStringLiteral("会话"), QStringLiteral("w1")));
	QCOMPARE(catalog.workspaceFor(QStringLiteral("s1")), QStringLiteral("w1"));
	QCOMPARE(catalog.visibleSessions().at(0).workspaceId, QStringLiteral("w1"));

	// 未指定工作区的新会话落在未分组（空 workspaceId）
	QVERIFY(catalog.addSession(QStringLiteral("s2"), QStringLiteral("会话2")));
	QCOMPARE(catalog.workspaceFor(QStringLiteral("s2")), QString());
}

void TestSessionCatalog::updateTitleOnlyForKnownSession()
{
	SessionCatalog catalog;

	QJsonArray items;
	items.append(session(QStringLiteral("s1"), QStringLiteral("旧标题")));
	catalog.setSessions(items);

	QVERIFY(catalog.updateTitle(QStringLiteral("s1"), QStringLiteral("新标题")));
	QCOMPARE(catalog.titleFor(QStringLiteral("s1")), QStringLiteral("新标题"));

	QVERIFY(!catalog.updateTitle(QStringLiteral("missing"), QStringLiteral("x")));
}

void TestSessionCatalog::applySnapshotKeepsArchivedWhenWorkspaceListFails()
{
	SessionCatalog catalog;
	catalog.setArchivedSessionIds({ QStringLiteral("a1") });

	SessionListSnapshot snapshot;
	snapshot.workspacesOk = false; // workspace.list 失败
	snapshot.sessionsOk = true;
	snapshot.sessions.append(session(QStringLiteral("s1"), QStringLiteral("会话一")));
	snapshot.sessions.append(session(QStringLiteral("a1"), QStringLiteral("已归档")));

	catalog.applySnapshot(snapshot);

	// 工作区被清空，但归档集合保留 → a1 仍然不显示
	QVERIFY(catalog.workspaces().isEmpty());
	QCOMPARE(catalog.visibleSessions().size(), 1);
	QCOMPARE(catalog.visibleSessions().at(0).sessionId, QStringLiteral("s1"));
}

void TestSessionCatalog::applySnapshotSkipsSessionsWhenSessionListFails()
{
	SessionCatalog catalog;

	SessionListSnapshot snapshot;
	snapshot.workspacesOk = true;
	snapshot.workspaces = parseArray(R"([{ "workspaceId": "w1", "sessionIds": ["s1"] }])");
	snapshot.sessionsOk = false; // session.list 失败
	snapshot.errorCode = QStringLiteral("boom");
	snapshot.errorMessage = QStringLiteral("失败");

	// 先塞入一个旧会话，确认失败时不会被清空
	QJsonArray oldItems;
	oldItems.append(session(QStringLiteral("old"), QStringLiteral("旧会话")));
	catalog.setSessions(oldItems);

	catalog.applySnapshot(snapshot);

	QCOMPARE(catalog.workspaces().size(), 1);
	QCOMPARE(catalog.visibleSessions().size(), 1);
	QCOMPARE(catalog.visibleSessions().at(0).sessionId, QStringLiteral("old"));
}