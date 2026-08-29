#pragma once

// ------------------------------------------------------------------
// Sidebar.h
// ------------------------------------------------------------------
// 左侧边栏相关类：
//   - SidebarLogo：Logo 控件
//   - NewWorkspaceButton：新建工作区按钮
//   - ClearSessionButton：清空会话按钮
//   - SidebarSettingsButton：设置按钮
//   - WorkspaceButton：工作区按钮
//   - WorkspaceList：工作区列表，按工作区管理会话，并对外提供会话聚合操作
//   - Sidebar：左侧边栏，统筹管理 Logo、按钮和 WorkspaceList，并转发会话相关信号
// ------------------------------------------------------------------

#include <QHash>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QString>
#include <QWidget>

#include <functional>
#include <vector>

class QVBoxLayout;
class QEnterEvent;
class QEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QContextMenuEvent;
class SessionButton;
class DshApiClient;
class SessionPrefetcher;

// 左侧边栏 Logo
class SidebarLogo : public QLabel
{
	Q_OBJECT

public:
	explicit SidebarLogo(QWidget* parent = nullptr);
};

// “新建工作区”按钮，独立成类
class NewWorkspaceButton : public QPushButton
{
	Q_OBJECT

public:
	explicit NewWorkspaceButton(QWidget* parent = nullptr);
};

// “清空会话”按钮，独立成类
class ClearSessionButton : public QPushButton
{
	Q_OBJECT

public:
	explicit ClearSessionButton(QWidget* parent = nullptr);
};

// 左侧边栏“设置”按钮，独立成类
class SidebarSettingsButton : public QPushButton
{
	Q_OBJECT

public:
	explicit SidebarSettingsButton(QWidget* parent = nullptr);
};

// 左侧边栏“插件”按钮，独立成类
class SidebarPluginsButton : public QPushButton
{
	Q_OBJECT

public:
	explicit SidebarPluginsButton(QWidget* parent = nullptr);
};

// 左侧边栏“主题”按钮，独立成类
class SidebarThemeButton : public QPushButton
{
	Q_OBJECT

public:
	explicit SidebarThemeButton(QWidget* parent = nullptr);
};

// 左侧边栏“Extension 管理”按钮，独立成类
class SidebarExtensionButton : public QPushButton
{
	Q_OBJECT

public:
	explicit SidebarExtensionButton(QWidget* parent = nullptr);
};

// 会话列表中的单个会话按钮
class SessionButton : public QPushButton
{
	Q_OBJECT

public:
	explicit SessionButton(const QString& sessionId,
		const QString& title,
		QWidget* parent = nullptr);

	QString sessionId() const;

	// 原始完整标题
	QString fullTitle() const;

	// 更新显示标题（内部会重新计算省略号文本）
	void setSessionTitle(const QString& title);

	// 设置当前选中状态
	void setSelected(bool selected);

signals:
	// 点击该会话按钮时发出
	void sessionClicked(const QString& sessionId);
	// 右键菜单请求删除会话
	void deleteRequested(const QString& sessionId);

protected:
	void resizeEvent(QResizeEvent* event) override;
	void contextMenuEvent(QContextMenuEvent* event) override;

private slots:
	void handleClicked();

private:
	void updateElidedText();

	QString m_sessionId;
	QString m_fullTitle;
};

// 工作区按钮：点击可折叠/展开该工作区下的会话，右侧绘制加号
class WorkspaceButton : public QPushButton
{
	Q_OBJECT

public:
	explicit WorkspaceButton(const QString& title, QWidget* parent = nullptr);
	void setExpanded(bool expanded);

signals:
	void addSessionRequested();

protected:
	void enterEvent(QEnterEvent* event) override;
	void leaveEvent(QEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void mouseReleaseEvent(QMouseEvent* event) override;
	void paintEvent(QPaintEvent* event) override;

private:
	QString m_title;
	bool m_plusHovered = false;
};

// 工作区列表：负责按工作区管理会话，并对外提供会话相关的聚合操作
class WorkspaceList : public QWidget
{
	Q_OBJECT

public:
	explicit WorkspaceList(QWidget* parent = nullptr);

	void setWorkspaces(const QJsonArray& items);
	void setArchivedSessionIds(const QSet<QString>& ids);
	void addSession(const QString& sessionId, const QString& title);
	void addSessionToWorkspace(const QString& sessionId, const QString& title, const QString& workspaceId);
	void clearSessions();
	void setCurrentSession(const QString& sessionId);
	void updateSessionTitle(const QString& sessionId, const QString& title);
	QString titleForSession(const QString& sessionId) const;
	void refreshTitles(DshApiClient* api);

signals:
	void sessionSelected(const QString& sessionId);
	void createSessionInWorkspaceRequested(const QString& workspaceId);
	void deleteSessionRequested(const QString& sessionId);

private:
	struct WorkspaceGroup
	{
		QString workspaceId;
		WorkspaceButton* header = nullptr;
		QWidget* container = nullptr;
		QVBoxLayout* layout = nullptr;
		std::vector<SessionButton*> buttons;
		bool expanded = true;
	};

	WorkspaceGroup* createWorkspaceGroup(const QString& workspaceId, const QString& title);
	WorkspaceGroup* defaultGroup();
	void clearWorkspaceGroups();

	QVBoxLayout* m_layout = nullptr;
	std::vector<SessionButton*> m_buttons;
	std::vector<WorkspaceGroup*> m_workspaceGroups;
	QHash<QString, QString> m_sessionWorkspace;
	QSet<QString> m_archivedSessionIds;
	WorkspaceGroup* m_defaultGroup = nullptr;
};

// 左侧边栏：统筹管理 Logo、按钮和 WorkspaceList，并转发会话相关信号
class Sidebar : public QWidget
{
	Q_OBJECT

public:
	explicit Sidebar(QWidget* parent = nullptr);

	// 让外部可以直接操作真正的会话管理者
	WorkspaceList* workspaceList() const;
	// 由 DSHHub 调用：设置工作区列表
	void setWorkspaces(const QJsonArray& items);
	// 由 DSHHub 调用：把 session.list 的 items 解析并填充到工作区/会话列表
	void setSessions(const QJsonArray& items);
	// 新建会话成功后，在侧边栏添加并选中该会话
	void addCreatedSession(const QString& sessionId, const QString& workspaceId = QString());

	// refresh workspaces/sessions
	void refreshSessions(DshApiClient* api, SessionPrefetcher* prefetcher);
	void createSession(DshApiClient* api, const QString& workspaceId = QString());

	// 清空全部会话：文件处理由 Sidebar 自己负责，会话列表清理由 WorkspaceList 负责
	void clearAllSessions(
		const QString& dshHome,
		const std::function<void()>& onCleared,
		const std::function<void()>& onCreateNew);

signals:
	void newWorkspaceRequested();
	void createSessionInWorkspaceRequested(const QString& workspaceId);
	void sessionSelected(const QString& sessionId);
	void deleteSessionRequested(const QString& sessionId);
	void clearRequested();
	void settingsRequested();
	void pluginsRequested();
	void themeToggleRequested();
	void extensionsRequested();

	void initialSessionReady(const QString& sessionId, const QString& title);
	void sessionCreated(const QString& sessionId, const QString& workspaceId);
	void noSessionAvailable();
	void sessionListError(const QString& code, const QString& message);
	void sessionCreateError(const QString& code, const QString& message);

private:
	WorkspaceList* m_workspaceList = nullptr;
	SidebarLogo* m_logo = nullptr;
	ClearSessionButton* m_clearButton = nullptr;
	NewWorkspaceButton* m_newWorkspaceButton = nullptr;
	SidebarSettingsButton* m_settingsButton = nullptr;
	SidebarPluginsButton* m_pluginsButton = nullptr;
	SidebarThemeButton* m_themeButton = nullptr;
	SidebarExtensionButton* m_extensionButton = nullptr;
	QVBoxLayout* m_layout = nullptr;
};