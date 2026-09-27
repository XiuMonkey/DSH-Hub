#pragma once

// 工作区（workspace）基线与命令的唯一归属：缓存 workspace/follow 的基线清单与归档集合、回答"新会话该挂
// 哪个分组"，并把 workspace/create、workspace/delete 的编排收在这里。纯逻辑，不依赖 Qt Widget：基线变化
// 只发一次通知（setOnChanged），侧栏怎么重建由宿主决定。

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>

class DshApiClient;
class QObject;

class WorkplaceManager
{
public:
	// context 只用于删除 / 补建的超时兜底定时器；api 允许为空，此时所有命令直接走失败分支
	WorkplaceManager(QObject* context, DshApiClient* api);

	// 基线变化（快照 / 增量 / 清空）后触发一次，宿主在这里把基线推给侧栏
	void setOnChanged(const std::function<void()>& onChanged);

	// workspace/follow 的全量快照：items 与 archivedSessionIds 都是整体替换
	void applySnapshot(const QJsonArray& items, const QJsonArray& archivedSessionIds);
	// upsert：同 id 替换，否则追加
	void upsert(const QJsonObject& workspace);
	void remove(const QString& workspaceId);
	// order：按服务端给的完整顺序重排；没被点名的接在后面，避免丢工作区
	void reorder(const QStringList& workspaceIds);
	// archived：归档集合整体替换
	void setArchived(const QJsonArray& archivedSessionIds);
	// 只清本地基线（清空会话时用）——服务端注册表要另外逐个 workspace/delete，见 deleteAllThen
	void clearBaseline();

	const QJsonArray& items() const { return m_items; }
	const QJsonArray& archivedSessionIds() const { return m_archived; }

	// 基线里全部非空 workspaceId，按基线顺序
	QStringList workspaceIds() const;
	// 基线里的第一个工作区；一个都没有返回空串
	QString firstWorkspaceId() const;

	// 把基线里每个工作区都对服务端 workspace/delete 一次，删完（或超时兜底）再回调。必须先删完再建会话 ——
	// 反过来的话 workspace/create 可能被随后到达的 delete 抹掉，新会话就挂在一个已删除的分组上了。
	void deleteAllThen(const std::function<void()>& onDone);
	// 一个分组都没有时按 path 补建默认工作区，建好后本地先行 upsert 再回调它的 id；失败回调空串
	void ensureDefaultThen(const QString& path, const std::function<void(const QString&)>& onResolved);

private:
	void notifyChanged();

	DshApiClient* m_api = nullptr;
	QObject* m_context = nullptr;
	std::function<void()> m_onChanged;

	QJsonArray m_items;
	QJsonArray m_archived;
};
