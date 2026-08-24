#pragma once

// ------------------------------------------------------------------
// WorkspaceSessionList.h
// ------------------------------------------------------------------
// 左侧会话列表面板相关类：
//   - WorkspaceButton：工作区按钮
//   - WorkspaceList：工作区列表，按工作区管理会话
//   - SessionList：左侧会话列表面板
// ------------------------------------------------------------------

#include <QHash>
#include <QJsonArray>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include <functional>
#include <vector>

class QVBoxLayout;
class QEnterEvent;
class QEvent;
class QMouseEvent;
class QPaintEvent;
class SessionButton;
class DshApiClient;

// “新建工作区”按钮，独立成类
class NewWorkspaceButton : public QPushButton
{
    Q_OBJECT

public:
    explicit NewWorkspaceButton(QWidget *parent = nullptr);
};

// “清空对话”按钮，独立成类
class ClearSessionButton : public QPushButton
{
    Q_OBJECT

public:
    explicit ClearSessionButton(QWidget *parent = nullptr);
};

// 工作区按钮：点击可折叠/展开该工作区下的会话，右侧绘制加号
class WorkspaceButton : public QPushButton
{
    Q_OBJECT

public:
    explicit WorkspaceButton(const QString &title, QWidget *parent = nullptr);
    void setExpanded(bool expanded);

signals:
    void addSessionRequested();

protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_title;
    bool m_expanded = true;
    bool m_showPlus = true;
    bool m_plusHovered = false;
};

// 工作区列表：负责按工作区管理 SessionButton
class WorkspaceList : public QWidget
{
    Q_OBJECT

public:
    explicit WorkspaceList(QWidget *parent = nullptr);

    void setWorkspaces(const QJsonArray &items);
    void addSession(const QString &sessionId, const QString &title);
    void addSessionToWorkspace(const QString &sessionId, const QString &title, const QString &workspaceId);
    void clearSessions();
    void setCurrentSession(const QString &sessionId);
    void updateSessionTitle(const QString &sessionId, const QString &title);
    QString titleForSession(const QString &sessionId) const;

signals:
    void sessionSelected(const QString &sessionId);
    void createSessionInWorkspaceRequested(const QString &workspaceId);

private:
    struct WorkspaceGroup
    {
        QString workspaceId;
        WorkspaceButton *header = nullptr;
        QWidget *container = nullptr;
        QVBoxLayout *layout = nullptr;
        std::vector<SessionButton *> buttons;
        bool expanded = true;
    };

    WorkspaceGroup *createWorkspaceGroup(const QString &workspaceId, const QString &title);
    WorkspaceGroup *defaultGroup();
    void clearWorkspaceGroups();

    QVBoxLayout *m_layout = nullptr;
    std::vector<SessionButton *> m_buttons;
    std::vector<WorkspaceGroup *> m_workspaceGroups;
    QHash<QString, QString> m_sessionWorkspace;
    WorkspaceGroup *m_defaultGroup = nullptr;
};

// 左侧会话列表面板
class SessionList : public QWidget
{
    Q_OBJECT

public:
    explicit SessionList(QWidget *parent = nullptr);

    void setWorkspaces(const QJsonArray &items);
    void addSession(const QString &sessionId, const QString &title);
    void addSessionToWorkspace(const QString &sessionId, const QString &title, const QString &workspaceId);
    void clearSessions();
    void setCurrentSession(const QString &sessionId);
    void updateSessionTitle(const QString &sessionId, const QString &title);
    QString titleForSession(const QString &sessionId) const;
    void refreshTitles(DshApiClient *api);
    void clearAllSessions(
        const QString &dshHome,
        const std::function<void()> &onCleared,
        const std::function<void()> &onCreateNew);

signals:
    void newWorkspaceRequested();
    void createSessionInWorkspaceRequested(const QString &workspaceId);
    void sessionSelected(const QString &sessionId);
    void clearRequested();
    void settingsRequested();

private:
    WorkspaceList *m_workspaceList = nullptr;
    ClearSessionButton *m_clearButton = nullptr;
    NewWorkspaceButton *m_newWorkspaceButton = nullptr;
    QPushButton *m_settingsButton = nullptr;
    QVBoxLayout *m_layout = nullptr;
};

