#include "common/workspace/WorkplaceManager.h"
#include "common/session/SessionCommands.h"
#include "network/DshApiClient.h"

#include <QDebug>
#include <QDir>
#include <QSet>
#include <QTimer>

#include <memory>

WorkplaceManager::WorkplaceManager(QObject* context, DshApiClient* api)
	: m_api(api)
	, m_context(context)
{
}

void WorkplaceManager::setOnChanged(const std::function<void()>& onChanged)
{
	m_onChanged = onChanged;
}

void WorkplaceManager::notifyChanged()
{
	if (m_onChanged)
		m_onChanged();
}

void WorkplaceManager::applySnapshot(const QJsonArray& items, const QJsonArray& archivedSessionIds)
{
	qInfo().noquote() << "[DSH Hub] workspace baseline items=" << items.size()
		<< "archived=" << archivedSessionIds.size();
	m_items = items;
	m_archived = archivedSessionIds;
	notifyChanged();
}

void WorkplaceManager::upsert(const QJsonObject& workspace)
{
	const QString workspaceId = workspace.value(QStringLiteral("workspaceId")).toString();
	if (workspaceId.isEmpty())
		return;

	bool replaced = false;
	for (int i = 0; i < m_items.size(); ++i) {
		const QString existing = m_items.at(i).toObject()
			.value(QStringLiteral("workspaceId")).toString();
		if (existing == workspaceId) {
			m_items.replace(i, workspace);
			replaced = true;
			break;
		}
	}
	if (!replaced)
		m_items.append(workspace);

	notifyChanged();
}

void WorkplaceManager::remove(const QString& workspaceId)
{
	if (workspaceId.isEmpty())
		return;

	QJsonArray kept;
	for (const auto& item : m_items) {
		if (item.toObject().value(QStringLiteral("workspaceId")).toString() != workspaceId)
			kept.append(item);
	}
	m_items = kept;
	notifyChanged();
}

void WorkplaceManager::reorder(const QStringList& workspaceIds)
{
	if (workspaceIds.isEmpty())
		return;

	QJsonArray ordered;
	QSet<QString> used;
	for (const QString& workspaceId : workspaceIds) {
		for (const auto& item : m_items) {
			const QJsonObject workspace = item.toObject();
			if (workspace.value(QStringLiteral("workspaceId")).toString() != workspaceId)
				continue;
			ordered.append(workspace);
			used.insert(workspaceId);
			break;
		}
	}

	// 服务端没点名的接在后面，避免丢工作区
	for (const auto& item : m_items) {
		const QString workspaceId = item.toObject()
			.value(QStringLiteral("workspaceId")).toString();
		if (!used.contains(workspaceId))
			ordered.append(item);
	}

	qInfo().noquote() << "[DSH Hub] workspace order update ids=" << workspaceIds.size();
	m_items = ordered;
	notifyChanged();
}

void WorkplaceManager::setArchived(const QJsonArray& archivedSessionIds)
{
	m_archived = archivedSessionIds;
	notifyChanged();
}

void WorkplaceManager::clearBaseline()
{
	m_items = QJsonArray();
	notifyChanged();
}

QStringList WorkplaceManager::workspaceIds() const
{
	QStringList ids;
	for (const auto& item : m_items) {
		const QString workspaceId = item.toObject().value(QStringLiteral("workspaceId")).toString();
		if (!workspaceId.isEmpty())
			ids.append(workspaceId);
	}
	return ids;
}

QString WorkplaceManager::firstWorkspaceId() const
{
	for (const auto& item : m_items) {
		const QString workspaceId = item.toObject().value(QStringLiteral("workspaceId")).toString();
		if (!workspaceId.isEmpty())
			return workspaceId;
	}
	return QString();
}

// 注册表活在服务端内存里，删 workspace.json 当次无效（之后任何变更都会把它整体写回来），所以逐个删。
// 回包收不齐时按超时兜底，不能让"新建"吊死。
void WorkplaceManager::deleteAllThen(const std::function<void()>& onDone)
{
	const QStringList ids = workspaceIds();

	// 本地基线先清：侧栏立刻空掉，也保证后面取不到旧 id
	clearBaseline();

	if (ids.isEmpty() || !m_api) {
		onDone();
		return;
	}

	auto called = std::make_shared<bool>(false);
	auto finish = [called, onDone]() {
		if (*called)
			return;
		*called = true;
		onDone();
		};
	auto remaining = std::make_shared<int>(static_cast<int>(ids.size()));
	for (const QString& workspaceId : ids) {
		m_api->callMethod(QStringLiteral("workspace/delete"), SessionCommands::workspaceDelete(workspaceId),
			[workspaceId, remaining, finish](const QJsonObject&) {
				qInfo().noquote() << QStringLiteral("[DSH Hub] 工作区已删除: %1").arg(workspaceId);
				if (--(*remaining) == 0)
					finish();
			},
			[workspaceId, remaining, finish](const DshApiClient::RpcError& error) {
				qWarning().noquote() << QStringLiteral("[DSH Hub] 工作区删除失败，新会话不会再挂到它: %1")
					.arg(workspaceId) << error.code << error.message;
				if (--(*remaining) == 0)
					finish();
			});
	}

	QTimer::singleShot(3000, m_context, finish);
}

// 一个分组都没有（全新安装的第一次会话、或注册表被清空过）时先补建默认工作区：session/create 不带
// workspaceId 时服务端**不会**把会话挂进任何工作区，侧栏只能回落到未分组；而且空注册表会一直空下去，
// 不是这一次的问题。
void WorkplaceManager::ensureDefaultThen(const QString& path, const std::function<void(const QString&)>& onResolved)
{
	if (!m_api) {
		onResolved(QString());
		return;
	}

	m_api->callMethod(QStringLiteral("workspace/create"), SessionCommands::workspaceCreate(path),
		[this, onResolved, path](const QJsonObject& value) {
			// 回包形状 { workspace: { workspaceId, title, sessionIds }, created }
			const QString created = value.value(QStringLiteral("workspace")).toObject()
				.value(QStringLiteral("workspaceId")).toString();
			if (created.isEmpty()) {
				qWarning().noquote() << QStringLiteral("[DSH Hub] workspace/create 回包没有 workspaceId，新会话将落到未分组");
				onResolved(QString());
				return;
			}

			// 本地先补一条工作区记录再往下走：workspace/upserted 是异步推送，等它到就晚了 ——
			// 分组不在 catalog 里时 addSession 找不到归属，照样是未分组。
			QJsonObject workspace;
			workspace.insert(QStringLiteral("workspaceId"), created);
			workspace.insert(QStringLiteral("title"), QDir(path).dirName());
			workspace.insert(QStringLiteral("sessionIds"), QJsonArray());
			upsert(workspace);

			qInfo().noquote() << QStringLiteral("[DSH Hub] 默认工作区就绪: %1 <- %2").arg(created, path);
			onResolved(created);
		},
		[onResolved](const DshApiClient::RpcError& error) {
			// 补建失败也要建会话：宁可落到未分组，也不能让"点新建"没反应
			qWarning().noquote() << QStringLiteral("[DSH Hub] 默认工作区补建失败，新会话将落到未分组:")
				<< error.code << error.message;
			onResolved(QString());
		});
}