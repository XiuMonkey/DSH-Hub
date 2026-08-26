#pragma once

// ------------------------------------------------------------------
// DSHHub.h
// ------------------------------------------------------------------
// 主窗口：DSH 富文本展示客户端。
// 启动时自动连接 DSH，支持发送消息，并用 AgentMessageUnit 渲染
// Markdown 回复、代码块、表格等富文本内容。
// ------------------------------------------------------------------

#include <QJsonObject>
#include <QJsonArray>
#include "CacheHistoryManager.h"

#include <QMainWindow>
#include <QProcess>
#include <QString>
#include <QStringList>

class DshApiClient;
class QLabel;
class QResizeEvent;
class SessionPrefetcher;
class ChatInputWidget;
class Sidebar;
class TopBar;
class Settings;
class PluginsPopup;
class QPushButton;
class AgentMessageUnit;
class MessageQuery;
class QVBoxLayout;
class QScrollArea;
class QTimer;
class LoadingCard;
class LoadMoreButton;

/**
 * DSH Hub 主窗口。
 */
class DSHHub : public QMainWindow
{
    Q_OBJECT

public:
    explicit DSHHub(QWidget *parent = nullptr);
    ~DSHHub() override;

    bool isInitializationComplete() const;

signals:
    void initializationComplete();

private slots:
    // 点击发送按钮
    void onSendClicked();

    // 新建工作区
    void onNewWorkspaceClicked();
    // 在指定工作区内新建会话
    void onCreateSessionInWorkspace(const QString &workspaceId);
    // 选择对话
    void onSessionSelected(const QString &sessionId);
    // 清空当前对话
    void onClearConversationClicked();
    void onLoadMoreHistory();
    void toggleTheme();
    // DSH 连接成功

    void handleConnected();
    // DSH 连接断开
    void handleDisconnected();
    // 收到 Mux 流消息
    void handleMuxFrame(const QJsonObject &frame);
    // 收到 Host 流消息
    void handleHostFrame(const QJsonObject &frame);
    // 传输错误
    void handleTransportError(const QString &context, const QString &message);

private:
    // 确保有可用的会话：先列会话，没有再创建
    void ensureSession();
    // 调用 workspace.list + session.list，并按工作区分组
    void callSessionList();
    // 获取工作区后加载会话列表
    void loadSessionList();
    // 调用 session.create
    // 加载会话历史（底部若干条）
    void loadHistory();

    // 显示/隐藏居中的“正在加载会话”指示器
    void showLoadingIndicator();
    void hideLoadingIndicator();
    // 把历史 events 渲染成消息；prepend=true 时插入到现有消息前面
    void renderHistoryEvents(const QJsonArray &events, bool prepend);
    // 离屏构建完整消息控件，构建完成后再一次性切换
    MessageQuery *buildOffscreenQuery(const QJsonArray &events);
    // 增量构建完整历史消息，避免首次加载一次性创建大量控件导致卡顿
    void startIncrementalHistoryBuild(const QJsonArray &events);
    void continueIncrementalHistoryBuild();
    void cancelIncrementalHistoryBuild();
    // 用离屏构建好的 MessageQuery 替换当前显示，并尽量减少闪烁
    void swapToMessageQuery(MessageQuery *query);
    // 初始化完成后隐藏灰色蒙版
    void finishInitialization();
    void resizeEvent(QResizeEvent *event) override;

    // 立即滚到底部（在界面刷新前同步完成，避免先显示顶部再闪烁）
    void scrollToBottomNow();

    // 设置弹窗
    void openSettings();
    void closeSettings();
    // 插件弹窗
    void openPlugins();

    // 控件缓存：把当前会话的消息控件从布局摘下并保存
    void cacheCurrentMessages();
    // 后台预取完成：保存最近几条历史，供打开会话时快速显示
    void onHistoryPrefetched(const QString &sessionId, const QJsonArray &events);
    // 用预取到的少量历史，在空闲时分批构建消息树缓存
    void processPrebuildQueue();

    // 尝试从缓存恢复某个会话的消息控件；成功返回 true
    bool tryRestoreCachedMessages(const QString &sessionId);

    // 显示“没有更多了”底部提示
    void showNoMoreToast();
    void callSessionCreate();
    // 启动内置的 DSH 服务端
    void startBundledServer();
    void launchBundledServer(const QString &nodePath,
                             const QString &entryPath,
                             const QString &dshEntry,
                             const QString &cwd,
                             const QString &dshHome);
    // 读取服务端输出，解析实际端口
    void handleServerOutput();
    // 服务端进程退出
    void handleServerFinished(int exitCode, QProcess::ExitStatus exitStatus);

    // 历史消息加载状态
    HistoryManager m_history;
    bool m_usingPrefetched = false;

    // 当前选中的会话 ID
    QString m_sessionId;

    // 当前 DSH 数据目录（用于本地删除 session）
    QString m_dshHome;

    // 是否正在通过 assistant/chunk 流式输出
    bool m_streaming = false;

    // 流式渲染节流定时器，避免每个 chunk 都全量重渲染导致卡顿
    QTimer *m_streamTimer = nullptr;

    // 内置 DSH 服务端进程
    QProcess *m_serverProcess = nullptr;

    // 界面控件
    CacheManager m_cacheManager;
    SessionPrefetcher *m_prefetcher = nullptr;

    QLabel *m_statusLabel = nullptr;
    QWidget *m_initOverlay = nullptr;
    QLabel *m_initLabel = nullptr;
    QWidget *m_settingsOverlay = nullptr;
    Settings *m_settings = nullptr;
    QWidget *m_pluginsOverlay = nullptr;
    PluginsPopup *m_pluginsPopup = nullptr;

    bool m_initializationComplete = false;
    bool m_scrollToBottomScheduled = false;

    QScrollArea *m_scrollArea = nullptr;

    LoadMoreButton *m_loadMoreButton = nullptr;
    QLabel *m_toastLabel = nullptr;
    LoadingCard *m_loadingCard = nullptr;
    QWidget *m_loadingContainer = nullptr;
    MessageQuery *m_messages = nullptr;
    TopBar *m_topBar = nullptr;

    // 增量构建历史消息时的临时状态
    QWidget *m_pendingBuildHolder = nullptr;
    QVBoxLayout *m_pendingBuildLayout = nullptr;
    MessageQuery *m_pendingBuildQuery = nullptr;
    QJsonArray m_pendingBuildEvents;
    int m_pendingBuildIndex = 0;
    bool m_pendingBuildActive = false;

    // 初始化阶段预构建消息树缓存的队列
    QStringList m_prebuildQueue;
    bool m_prebuilding = false;

    Sidebar *m_sidebar = nullptr;

    QVBoxLayout *m_messagesLayout = nullptr;
    ChatInputWidget *m_chatInput = nullptr;

    // DSH API 客户端
    DshApiClient *m_api = nullptr;
};

