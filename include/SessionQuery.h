#pragma once

// ------------------------------------------------------------------
// SessionQuery.h
// ------------------------------------------------------------------
// 左侧灰色会话列表面板：
//   - 上方是“新建会话”按钮
//   - 下方纵向罗列 SessionButton
// 内部持有 std::vector<SessionButton*> 来管理会话按钮。
// 注意：SessionButton 继承 QObject，不能直接按值放入 std::vector，
// 所以这里使用指针数组管理。
// ------------------------------------------------------------------

#include <QPushButton>
#include <QString>
#include <QWidget>

#include <functional>
#include <vector>

class QVBoxLayout;
class SessionButton;
class DshApiClient;

// “新建对话”按钮，独立成类，不与会话列表面板逻辑混在一起
class NewSessionButton : public QPushButton
{
    Q_OBJECT

public:
    explicit NewSessionButton(QWidget *parent = nullptr);
};

// “清空对话”按钮，独立成类
class ClearSessionButton : public QPushButton
{
    Q_OBJECT

public:
    explicit ClearSessionButton(QWidget *parent = nullptr);
};

class SessionQuery : public QWidget
{
    Q_OBJECT

public:
    explicit SessionQuery(QWidget *parent = nullptr);

    // 追加一个会话
    void addSession(const QString &sessionId, const QString &title);

    // 清空所有会话按钮
    void clearSessions();

    // 高亮当前选中的会话
    void setCurrentSession(const QString &sessionId);

    // 更新某个会话的显示标题
    void updateSessionTitle(const QString &sessionId, const QString &title);

    // 获取某个会话的完整标题
    QString titleForSession(const QString &sessionId) const;

    // 从服务端刷新所有会话标题
    void refreshTitles(DshApiClient *api);

    // 新建会话
    void requestNewSession(
        DshApiClient *api,
        const std::function<void()> &beforeCreated,
        const std::function<void(const QString &sessionId)> &onCreated,
        const std::function<void(const QString &code, const QString &message)> &onError);

    // 清空所有会话并删除本地 session 数据
    void clearAllSessions(
        const QString &dshHome,
        const std::function<void()> &onCleared,
        const std::function<void()> &onCreateNew);

signals:
    // 点击“清空会话”
    void clearRequested();

    // 点击“新建会话”
    void newSessionRequested();

    // 点击某个会话按钮
    void sessionSelected(const QString &sessionId);

    // 点击设置按钮
    void settingsRequested();

private:
    ClearSessionButton *m_clearButton = nullptr;
    NewSessionButton *m_newSessionButton = nullptr;
    QPushButton *m_settingsButton = nullptr;
    QVBoxLayout *m_layout = nullptr;
    std::vector<SessionButton *> m_buttons;
};
