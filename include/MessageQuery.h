#pragma once

#include "AgentMessageUnit.h"
#include "SystemMessageUnit.h"
#include "UserMessageUnit.h"

#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QWidget>

#include <vector>

class QVBoxLayout;
class DshApiClient;
class HistoryManager;
class QScrollArea;

class MessageQuery
{
public:
    struct MessageUnit
    {
        int type = 0; // 0: user, 1: agent, 2: system
        AgentMessageUnit *agentUnit = nullptr;
        UserMessageUnit *userUnit = nullptr;
        SystemMessageUnit *systemUnit = nullptr;
        QWidget *container = nullptr;
    };

    std::vector<MessageUnit> messages;

    MessageQuery();
    ~MessageQuery();

    UserMessageUnit *addUserMessage(const QString &text, QVBoxLayout *layout);
    SystemMessageUnit *addSystemMessage(const QString &text, QVBoxLayout *layout);
    AgentMessageUnit *addAgentMessage(const QString &markdown, QVBoxLayout *layout,
                                      const QString &thinking = QString());
    UserMessageUnit *insertUserMessage(const QString &text, QVBoxLayout *layout, int layoutIndex);
    AgentMessageUnit *insertAgentMessage(const QString &markdown, QVBoxLayout *layout, int layoutIndex,
                                         const QString &thinking = QString());
    AgentMessageUnit *lastAgentUnit() const;
    AgentMessageUnit *lastAgentUnitIfLast() const;
    void clear();

    // 把 DSH 历史事件渲染进当前消息列表
    void appendEvents(QVBoxLayout *layout, const QJsonArray &events);
    void prependEvents(QVBoxLayout *layout, const QJsonArray &events, int layoutIndex);
    void prependOlderEvents(QVBoxLayout *layout, const QJsonArray &events, int oldCount, int layoutIndex);

    // 从历史事件离线构建一个完整 MessageQuery，调用方负责后续 attach/释放
    static MessageQuery *fromEvents(const QJsonArray &events);

    // 控件缓存支持：从布局中摘下但不销毁，之后可以重新 attach 回来
    void detachFromLayout(QVBoxLayout *layout);
    void attachToLayout(QVBoxLayout *layout);

    // 把内部控件从当前父对象上解除，便于从临时离屏容器安全迁移
    void releaseWidgets();
};

// 离屏增量构建消息列表，避免一次性渲染大量历史控件卡顿。
class MessageQueryBuilder
{
public:
    MessageQueryBuilder();
    ~MessageQueryBuilder();

    // 开始用 events 构建一个新的 MessageQuery
    void start(const QJsonArray &events);
    // 处理下一批；返回 true 表示仍在构建，false 表示已完成
    bool step(int batchSize = 5);
    // 取消并释放临时控件
    void cancel();
    bool isActive() const;
    // 取出构建完成的 MessageQuery，调用方负责后续 attach/释放
    MessageQuery *takeResult();

private:
    QWidget *m_holder = nullptr;
    QVBoxLayout *m_layout = nullptr;
    MessageQuery *m_query = nullptr;
    QJsonArray m_events;
    int m_index = 0;
    bool m_active = false;
};

// 历史消息加载器：负责 session.history 拉取、增量构建、加载更多
class HistoryLoader : public QObject
{
    Q_OBJECT

public:
    HistoryLoader(DshApiClient *api,
                  MessageQuery *messages,
                  QVBoxLayout *layout,
                  HistoryManager *history,
                  QScrollArea *scrollArea,
                  QObject *parent = nullptr);

    void load(const QString &sessionId);
    void loadMore();
    void setUsingPrefetched(bool usingPrefetched);
    void setMessages(MessageQuery *messages);
    bool isBuilding() const;
    void cancelBuild();

signals:
    void loadingChanged(bool loading);
    void loadMoreButtonVisibleChanged(bool visible);
    void noMoreHistory();
    void historyError(const QString &code, const QString &message);
    void incrementalBuildReady(MessageQuery *query);
    void initializationFinished();

private:
    void continueBuild();
    void startBuild(const QJsonArray &events);

    DshApiClient *m_api = nullptr;
    MessageQuery *m_messages = nullptr;
    QVBoxLayout *m_layout = nullptr;
    HistoryManager *m_history = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    MessageQueryBuilder m_builder;
    QString m_sessionId;
    QString m_buildSessionId;
    int m_loadGeneration = 0;
    int m_buildGeneration = 0;
    bool m_usingPrefetched = false;
    bool m_loading = false;
};
