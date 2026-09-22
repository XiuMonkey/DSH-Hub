#pragma once

// 左侧边栏控件：Logo、各功能按钮、WorkspaceList 与 Sidebar 本体。
// 会话/工作区的数据与 RPC 逻辑在 common（SessionCatalog / SessionService），本文件只保留控件与绘制。

#include "common/session/SessionCatalog.h"

#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include <functional>
#include <vector>

class QScrollArea;
class QVBoxLayout;
class QEnterEvent;
class QEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QContextMenuEvent;
class DshApiClient;

class SidebarLogo : public QLabel
{
	Q_OBJECT

public:
	explicit SidebarLogo(QWidget* parent = nullptr);
};

class NewWorkspaceButton : public QPushButton
{
	Q_OBJECT

public:
	explicit NewWorkspaceButton(QWidget* parent = nullptr);
};

class ClearSessionButton : public QPushButton
{
	Q_OBJECT

public:
	explicit ClearSessionButton(QWidget* parent = nullptr);
};

class SidebarSettingsButton : public QPushButton
{
	Q_OBJECT

public:
	explicit SidebarSettingsButton(QWidget* parent = nullptr);
};

class SidebarPluginsButton : public QPushButton
{
	Q_OBJECT

public:
	explicit SidebarPluginsButton(QWidget* parent = nullptr);
};

class SidebarThemeButton : public QPushButton
{
	Q_OBJECT

public:
	explicit SidebarThemeButton(QWidget* parent = nullptr);
};

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

	// 更新显示标题（内部会重新计算省略号文本）
	void setSessionTitle(const QString& title);

	void setSelected(bool selected);

signals:
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

// 工作区按钮：点击折叠/展开该工作区下的会话，右侧绘制加号
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

// 工作区列表：按工作区分组渲染会话按钮，数据（分组/标题/归档状态）来自 SessionCatalog，本类只负责画。
class WorkspaceList : public QWidget
{
	Q_OBJECT

public:
	explicit WorkspaceList(QWidget* parent = nullptr);

	// 数据源：由 SessionService 在刷新时写入
	SessionCatalog& catalog();

	// 按 catalog 的当前内容整体重建
	void rebuildFromCatalog();

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
	WorkspaceGroup* groupFor(const QString& workspaceId);
	// 在对应工作区分组下创建（或更新）一个会话按钮
	void addSessionButton(const QString& sessionId, const QString& title);
	void clearWorkspaceGroups();

	QVBoxLayout* m_layout = nullptr;
	std::vector<SessionButton*> m_buttons;
	std::vector<WorkspaceGroup*> m_workspaceGroups;
	SessionCatalog m_catalog;
	WorkspaceGroup* m_defaultGroup = nullptr;
};

// 左侧边栏：统筹管理 Logo、按钮和 WorkspaceList，并转发会话相关信号
class Sidebar : public QWidget
{
	Q_OBJECT

public:
	explicit Sidebar(QWidget* parent = nullptr);
	// 析构时把自己从全局注册表摘掉（Destroy 带身份校验，见 CommonRegistry.h）
	~Sidebar() override;

	// 让外部可以直接操作真正的会话管理者
	WorkspaceList* workspaceList() const;
	// 新建会话成功后，在侧边栏添加并选中该会话
	void addCreatedSession(const QString& sessionId, const QString& workspaceId = QString());

	// 数据解析由 SessionService 负责，本类只把结果映射成界面状态并转发信号
	void refreshSessions(DshApiClient* api);
	void createSession(DshApiClient* api, const QString& workspaceId = QString());

	// 文件与目录清理由 SessionService 负责，会话列表的清理由 WorkspaceList（catalog）负责
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

	// 会话列表已刷新并写入 catalog（DSHHub 用它触发首屏预取）
	void sessionsRefreshed();
	void initialSessionReady(const QString& sessionId, const QString& title);
	void sessionCreated(const QString& sessionId, const QString& workspaceId);
	void noSessionAvailable();
	void sessionListError(const QString& code, const QString& message);
	void sessionCreateError(const QString& code, const QString& message);

private:
	// 会话列表的滚动容器：列表内容再长也只滚动，不参与撑高侧栏
	QScrollArea* m_workspaceScroll = nullptr;
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