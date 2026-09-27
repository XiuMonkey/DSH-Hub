#pragma once

// 左侧边栏控件：Logo、功能按钮、WorkspaceList 与 Sidebar 本体；数据与 RPC 逻辑在 common，本文件只画。

#include "common/session/SessionCatalog.h"
#include "VirtualClass/VirtualCommon.h"

#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QWidget>
#include <functional>
#include <vector>

class QHBoxLayout;
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

class SessionButton : public QPushButton
{
	Q_OBJECT

public:
	explicit SessionButton(const QString& sessionId, const QString& title, QWidget* parent = nullptr);

	QString sessionId() const;
	void setSessionTitle(const QString& title);
	void setSelected(bool selected);

signals:
	void sessionClicked(const QString& sessionId);
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

// 工作区列表：按工作区分组渲染会话按钮，数据来自 SessionCatalog，本类只负责画
class WorkspaceList : public QWidget
{
	Q_OBJECT

public:
	explicit WorkspaceList(QWidget* parent = nullptr);

	SessionCatalog& catalog();
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
	void addSessionButton(const QString& sessionId, const QString& title);
	void clearWorkspaceGroups();

	QVBoxLayout* m_layout = nullptr;
	std::vector<SessionButton*> m_buttons;
	std::vector<WorkspaceGroup*> m_workspaceGroups;
	SessionCatalog m_catalog;
	WorkspaceGroup* m_defaultGroup = nullptr;
};

// 左侧边栏：统筹 Logo、按钮与 WorkspaceList，并转发会话相关信号
class Sidebar : public QWidget, public VirtualSidebar
{
	Q_OBJECT
		Q_INTERFACES(VirtualSidebar)

public:
	explicit Sidebar(QWidget* parent = nullptr);
	// 析构时把自己从全局注册表摘掉（Destroy 带身份校验）
	~Sidebar() override;

	WorkspaceList* workspaceList() const;
	void addCreatedSession(const QString& sessionId, const QString& workspaceId = QString());

	void refreshSessions(DshApiClient* api);
	void createSession(DshApiClient* api, const QString& workspaceId = QString());

	void clearAllSessions(const QString& dshHome, const std::function<void()>& onCleared,
		const std::function<void()>& onCreateNew);

	// 只管市场那一颗按钮：扩展管理（extensionsRequested）是扩展的装载通道，接管时绝不能跟着关
	void setPluginsEntryEnabled(bool enabled);

	// 交出侧栏底部那排图标按钮的横向布局（不转移所有权，插入的控件随后归侧栏所有）。
	// 跨 DLL 只能走 qt_metacast(IID) 或接口虚函数（别用 dynamic_cast，跨模块静默 nullptr；
	// 也别 qobject_cast<Sidebar*>，宿主符号零导出会 LNK2019）。指针只在侧栏存活期间有效。
	// 顺序为「设置 | 插件 | 主题 | 扩展 | 间距 | stretch」：addWidget 落在 stretch 之后（贴最右），
	// 想插进那排图标之间就自己 indexOf 定位。
	QHBoxLayout* GetNavLayout() override
	{
		return m_navLayout;
	}

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
	// 滚动容器：列表再长也只滚动，不撑高侧栏
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
	// 底部图标行。**必须是类级成员**：扩展经 VirtualSidebar::GetNavLayout() 拿它挂按钮
	// （写成构造里的局部变量时外面拿不到 —— TopBar 那边踩过同一个坑）
	QHBoxLayout* m_navLayout = nullptr;
};
