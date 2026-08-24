#include "DSHHub.h"
#include "ChatInputWidget.h"
#include "WorkspaceSessionList.h"

#include "DshApiClient.h"
#include "SessionPrefetcher.h"
#include "CodeHighlighter.h"
#include "SpinnerWidget.h"
#include "TopBar.h"
#include "Settings.h"


#include "DshEventParser.h"
#include "AgentMessageUnit.h"
#include "MessageQuery.h"
#include "LoadingCard.h"
#include "LoadMoreButton.h"

#include <QCoreApplication>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFrame>


#include <QHBoxLayout>

#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>



#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>

#include <QScrollBar>
#include <QScrollArea>

#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>


#include <QTimer>
#include <QUrl>
#include <QTcpSocket>
#include <QVBoxLayout>

namespace
{
// 把历史 events 追加到一个 MessageQuery + 布局里（非 prepend 模式共用逻辑）
void appendHistoryEventsToQuery(MessageQuery *query, QVBoxLayout *layout, const QJsonArray &events)
{
    for (const auto &value : events) {
        QJsonObject event = value.toObject();
        if (event.contains(QStringLiteral("event")) && event.value(QStringLiteral("event")).isObject())
            event = event.value(QStringLiteral("event")).toObject();

        const QString type = event.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("user/message")) {
            const QString text = extractEventText(event).trimmed();
            if (!text.isEmpty())
                query->addUserMessage(text, layout);
        } else if (type == QStringLiteral("assistant/message")) {
            const QString thinking = extractThinking(event);
            const QString reply = extractReply(event);
            if (!reply.isEmpty() || !thinking.isEmpty())
                query->addAgentMessage(reply, layout, thinking);
        }
    }
}
}

DSHHub::DSHHub(QWidget *parent)
    : QMainWindow(parent)
    , m_api(new DshApiClient(this))
{
    setWindowTitle(QStringLiteral("DSH Hub"));

    // 创建界面
    auto *central = new QWidget(this);
    central->setStyleSheet(QStringLiteral(
        "QWidget {"
        "  background: #F8F9FB;"
        "  color: #1F2328;"
        "}"
        "QLineEdit {"
        "  background: #FFFFFF;"
        "  border: 1px solid #D0D7DE;"
        "  border-radius: 8px;"
        "  padding: 6px 10px;"
        "}"
        "QLineEdit:focus {"
        "  border-color: #4C8BF5;"
        "}"
        "QPushButton {"
        "  background: #E8EAED;"
        "  border: none;"
        "  border-radius: 8px;"
        "  padding: 6px 14px;"
        "  color: #1F2328;"
        "}"
        "QPushButton:hover {"
        "  background: #DDE0E4;"
        "}"
        "QPushButton:pressed {"
        "  background: #CDD0D5;"
        "}"
    ));
    auto *layout = new QVBoxLayout(central);

    m_statusLabel = new QLabel(QStringLiteral("正在连接 DSH..."), central);
    m_statusLabel->hide(); // 不再显示顶部状态文字，仅保留变量避免改动其他逻辑

    m_scrollArea = new QScrollArea(central);
    m_scrollArea->setFixedWidth(900);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setStyleSheet(QStringLiteral(
        "QScrollArea {"
        "  background: #F8F9FB;"
        "  border: none;"
        "}"
        "QScrollArea QScrollBar:vertical {"
        "  background: transparent;"
        "  width: 8px;"
        "  margin: 2px;"
        "}"
        "QScrollArea QScrollBar::handle:vertical {"
        "  background: #C1C7CF;"
        "  border-radius: 4px;"
        "  min-height: 30px;"
        "}"
        "QScrollArea QScrollBar::handle:vertical:hover {"
        "  background: #A8B0B9;"
        "}"
        "QScrollArea QScrollBar::add-line:vertical,"
        "QScrollArea QScrollBar::sub-line:vertical {"
        "  height: 0;"
        "}"
        "QScrollArea QScrollBar::add-page:vertical,"
        "QScrollArea QScrollBar::sub-page:vertical {"
        "  background: transparent;"
        "}"
    ));

    auto *scrollContent = new QWidget;
    scrollContent->setStyleSheet(QStringLiteral("background: #F8F9FB;"));
    auto *scrollLayout = new QVBoxLayout(scrollContent);
    scrollLayout->setContentsMargins(0, 0, 0, 0);
    scrollLayout->setAlignment(Qt::AlignTop);

    m_messagesLayout = scrollLayout;
    m_messages = new MessageQuery;
    scrollContent->setLayout(scrollLayout);

    // 顶部“加载更多”按钮，默认隐藏
    m_loadMoreButton = new LoadMoreButton(scrollContent);
    m_loadMoreButton->hide();
    scrollLayout->insertWidget(0, m_loadMoreButton, 0, Qt::AlignHCenter);

    m_scrollArea->setWidget(scrollContent);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);


    // 右侧面板与主窗口同色
    auto *rightPanel = new QWidget(central);
    rightPanel->setFixedWidth(900);
    rightPanel->setAttribute(Qt::WA_StyledBackground, true);
    rightPanel->setStyleSheet(QStringLiteral(
        "background: #F8F9FB;"
        "border-radius: 12px;"
    ));

    m_chatInput = new ChatInputWidget(rightPanel);

    // 底部“没有更多了”提示
    m_toastLabel = new QLabel(QStringLiteral("啊哦，没有更多了"), this);
    m_toastLabel->setStyleSheet(QStringLiteral(
        "QLabel {"
        "  background: rgba(31,35,40,0.85);"
        "  color: white;"
        "  border-radius: 16px;"
        "  padding: 8px 16px;"
        "  font-size: 13px;"
        "}"
    ));
    m_toastLabel->setAlignment(Qt::AlignCenter);
    m_toastLabel->hide();


    auto *panelLayout = new QVBoxLayout(rightPanel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    // 左侧灰色会话列表
    m_sessionList = new SessionList(central);
    m_sessionList->setFixedWidth(240);

    panelLayout->addWidget(m_scrollArea);

    auto *inputLayout = new QHBoxLayout;
    inputLayout->addWidget(m_chatInput, 1);

    inputLayout->setContentsMargins(10, 8, 10, 8);
    inputLayout->setSpacing(8);
    panelLayout->addLayout(inputLayout);

    // 对话顶部栏：宽度与对话栏一致，放在右侧对话栏上方
    auto *rightColumn = new QWidget(central);
    rightColumn->setFixedWidth(900);
    auto *rightColumnLayout = new QVBoxLayout(rightColumn);
    rightColumnLayout->setContentsMargins(0, 0, 0, 0);
    rightColumnLayout->setSpacing(8);

    m_topBar = new TopBar(rightColumn);
    auto *topBarRow = new QWidget(rightColumn);
    auto *topBarRowLayout = new QHBoxLayout(topBarRow);
    topBarRowLayout->setContentsMargins(10, 0, 0, 0);
    topBarRowLayout->setSpacing(0);
    topBarRowLayout->addWidget(m_topBar);
    topBarRowLayout->addStretch();

    rightColumnLayout->addWidget(topBarRow);
    rightColumnLayout->addWidget(rightPanel, 1);

    auto *bodyLayout = new QHBoxLayout;
    bodyLayout->setSpacing(0);

    bodyLayout->addWidget(m_sessionList);
    bodyLayout->addWidget(rightColumn);

    // 内容容器：整体居中
    auto *content = new QWidget(central);
    content->setFixedWidth(1140);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->addLayout(bodyLayout, 1);

    layout->addWidget(content, 0, Qt::AlignHCenter);
    // 输入框已放入右侧白色面板，不再单独添加到主布局

    setCentralWidget(central);

    setMinimumWidth(1160);
    resize(1000, 700);

    // 初始化灰色蒙版 + 居中标签
    m_initOverlay = new QWidget(this);
    m_initOverlay->setObjectName(QStringLiteral("initOverlay"));
    m_initOverlay->setStyleSheet(QStringLiteral(
        "QWidget#initOverlay {"
        "  background-color: rgba(128,128,128,0.65);"
        "}"
    ));
    auto *overlayLayout = new QVBoxLayout(m_initOverlay);

    // 现代化横版卡片：宽高比约 5:3
    auto *initCard = new QWidget(m_initOverlay);
    initCard->setFixedSize(400, 240);
    initCard->setStyleSheet(QStringLiteral(
        "QWidget {"
        "  background: #FFFFFF;"
        "  border-radius: 20px;"
        "}"
    ));

    auto *cardLayout = new QVBoxLayout(initCard);
    cardLayout->setContentsMargins(24, 20, 24, 20);
    cardLayout->setSpacing(8);

    // 卡片内顶部显示 Logo
    auto *cardLogo = new QLabel(initCard);
    cardLogo->setAlignment(Qt::AlignCenter);
    cardLogo->setAttribute(Qt::WA_TranslucentBackground);
    QPixmap cardLogoPix(QStringLiteral(":/DSHHub/DSH-Hub-Logo-Tiny@2x.png"));
    if (!cardLogoPix.isNull()) {
        cardLogoPix.setDevicePixelRatio(2.0);
        cardLogo->setPixmap(cardLogoPix);
    } else {
        cardLogo->setText(QStringLiteral("DSH Hub"));
    }
    cardLayout->addWidget(cardLogo);

    // 下方：旋转条 + 初始化文字
    auto *rowLayout = new QHBoxLayout;
    rowLayout->setSpacing(16);

    auto *spinner = new SpinnerWidget(initCard);
    spinner->setFixedSize(40, 40);
    spinner->start();

    rowLayout->addStretch(1);
    rowLayout->addWidget(spinner, 0, Qt::AlignVCenter);

    m_initLabel = new QLabel(QStringLiteral("DSH Hub 正在初始化..."), initCard);
    m_initLabel->setAlignment(Qt::AlignCenter);
    m_initLabel->setStyleSheet(QStringLiteral(
        "QLabel {"
        "  background: transparent;"
        "  color: #1F2328;"
        "  font-size: 18px;"
        "  font-weight: 600;"
        "}"
    ));
    rowLayout->addWidget(m_initLabel, 0, Qt::AlignVCenter);
    rowLayout->addStretch(1);

    cardLayout->addLayout(rowLayout);
    cardLayout->addStretch(1);

    overlayLayout->addWidget(initCard, 0, Qt::AlignCenter);

    m_initOverlay->setGeometry(rect());
    m_initOverlay->raise();
    m_initOverlay->show();

    // 初始化时从 Qt 资源中加载代码高亮规则
    {
        if (!CodeHighlighter::instance().loadFromFile(QStringLiteral(":/DSHHub/highlight_rules.json"))) {
            qWarning().noquote() << QStringLiteral("[DSH Hub] 未找到内置资源 highlight_rules.json，代码高亮不可用");
        }
    }




    // 流式渲染节流：避免每个 chunk 都全量重渲染导致卡顿
    m_streamTimer = new QTimer(this);
    m_streamTimer->setInterval(50);
    m_streamTimer->setSingleShot(true);
    connect(m_streamTimer, &QTimer::timeout, this, [this]() {
        AgentMessageUnit *target = m_messages ? m_messages->lastAgentUnitIfLast() : nullptr;
        if (!target)
            return;
        target->flushStream();
        if (m_scrollArea && m_scrollArea->verticalScrollBar()) {
            QScrollBar *bar = m_scrollArea->verticalScrollBar();
            if (bar->value() >= bar->maximum() - 80)
                scrollToBottomNow();
        }
    });






    // 信号槽
    connect(m_chatInput, &ChatInputWidget::sendRequested, this, &DSHHub::onSendClicked);




    connect(m_sessionList, &SessionList::newWorkspaceRequested,
            this, &DSHHub::onNewWorkspaceClicked);
    connect(m_sessionList, &SessionList::createSessionInWorkspaceRequested,
            this, &DSHHub::onCreateSessionInWorkspace);
    connect(m_sessionList, &SessionList::sessionSelected,
            this, &DSHHub::onSessionSelected);
    connect(m_sessionList, &SessionList::clearRequested,
            this, &DSHHub::onClearConversationClicked);
    connect(m_loadMoreButton, &QPushButton::clicked, this, &DSHHub::onLoadMoreHistory);



    connect(m_api, &DshApiClient::connected, this, &DSHHub::handleConnected);
    connect(m_api, &DshApiClient::disconnected, this, &DSHHub::handleDisconnected);
    connect(m_api, &DshApiClient::muxFrameReceived, this, &DSHHub::handleMuxFrame);
    connect(m_api, &DshApiClient::hostFrameReceived, this, &DSHHub::handleHostFrame);
    connect(m_api, &DshApiClient::transportError, this, &DSHHub::handleTransportError);
    m_prefetcher = new SessionPrefetcher(this);
    connect(m_prefetcher, &SessionPrefetcher::historyFetched,
            this, &DSHHub::onHistoryPrefetched);
    connect(m_sessionList, &SessionList::settingsRequested,
            this, &DSHHub::openSettings);


    startBundledServer();
    // 等待内置服务端输出实际端口后再连接
}

void DSHHub::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);

    if (m_initOverlay)
        m_initOverlay->setGeometry(rect());

    if (m_settingsOverlay)
        m_settingsOverlay->setGeometry(rect());
}

void DSHHub::finishInitialization()
{
    if (m_initializationComplete)
        return;

    m_initializationComplete = true;

    if (m_initOverlay) {
        m_initOverlay->hide();
        m_initOverlay->deleteLater();
        m_initOverlay = nullptr;
        m_initLabel = nullptr;
    }
}

void DSHHub::openSettings()
{
    if (m_settings)
        return;

    // 灰色蒙版，和初始化蒙版一致，禁止主窗口交互
    m_settingsOverlay = new QWidget(this);
    m_settingsOverlay->setObjectName(QStringLiteral("settingsOverlay"));
    m_settingsOverlay->setAttribute(Qt::WA_StyledBackground, true);
    m_settingsOverlay->setStyleSheet(QStringLiteral(
        "QWidget#settingsOverlay {"
        "  background-color: rgba(128,128,128,0.65);"
        "}"
    ));
    m_settingsOverlay->setGeometry(rect());
    m_settingsOverlay->raise();
    m_settingsOverlay->show();

    m_settings = new Settings(m_dshHome, this);
    m_settings->move(geometry().center() - m_settings->rect().center());
    m_settings->show();

    connect(m_settings, &PopupWindow::closed, this, [this]() {
        closeSettings();
    });
}

void DSHHub::closeSettings()
{
    if (m_settingsOverlay) {
        m_settingsOverlay->hide();
        m_settingsOverlay->deleteLater();
        m_settingsOverlay = nullptr;
    }

    if (m_settings) {
        disconnect(m_settings, nullptr, this, nullptr);
        m_settings->close();
        m_settings->deleteLater();
        m_settings = nullptr;
    }
}



DSHHub::~DSHHub()
{
    if (m_api)
        disconnect(m_api, nullptr, this, nullptr);

    if (m_serverProcess && m_serverProcess->state() != QProcess::NotRunning) {
        m_serverProcess->kill();
        m_serverProcess->waitForFinished(2000);
    }

    delete m_messages;
    m_messages = nullptr;

    m_cacheManager.clearAll();

    // 退出时把完整统计写一次，覆盖首次加载时生成的快照

}






void DSHHub::startBundledServer()
{
    // 每次启动清空旧开销数据；这里记录从启动到 WebSocket 连通的整体耗时

    const QString appDir = QCoreApplication::applicationDirPath();

    //使用随客户端一起分发/内置的服务端
    QString nodePath = appDir + QStringLiteral("/resources/server/node.exe");
    QString entryPath = appDir + QStringLiteral("/resources/harness-node-entry.mjs");
    QString dshEntry = appDir + QStringLiteral("/resources/server/node_modules/@deepseek-ai/dsh/lib/bin.js");
    QString cwd = appDir + QStringLiteral("/resources/server/launch-root");
    QString dshHome = appDir + QStringLiteral("/resources/server/harness");
    m_connectionSource = QStringLiteral("内置服务");

    m_dshHome = dshHome;

    if (!QFile::exists(nodePath) || !QFile::exists(entryPath) || !QFile::exists(dshEntry)) {
        m_connectionSource = QStringLiteral("外部服务 (127.0.0.1:3080)");
        m_statusLabel->setText(QStringLiteral("未找到内置 DSH 服务端，尝试连接 3080..."));
        m_api->setBaseUrl(QUrl(QStringLiteral("http://127.0.0.1:3080")));

        m_api->openStreams();
        return;
    }
    // 使用独立的用户数据目录，避免继承 DSH Desktop 的聊天记录/会话
    QDir().mkpath(cwd);
    QDir().mkpath(dshHome);

    // 如果 3080 已经被占用，说明可能已有 DSH 服务在运行，直接连接，避免 EADDRINUSE
    {
        QTcpSocket probe;
        probe.connectToHost(QStringLiteral("127.0.0.1"), 3080);
        if (probe.waitForConnected(500)) {
            m_connectionSource = QStringLiteral("外部服务 (127.0.0.1:3080)");
            m_statusLabel->setText(QStringLiteral("检测到 3080 已有服务，直接连接..."));
            qDebug().noquote() << QStringLiteral("[DSH Hub] 使用端口: 3080");
            m_api->setBaseUrl(QUrl(QStringLiteral("http://127.0.0.1:3080")));

            m_api->openStreams();
            return;
        }
    }

    m_serverProcess = new QProcess(this);
    m_serverProcess->setProcessChannelMode(QProcess::MergedChannels);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("DSH_HOME"), dshHome);
    m_serverProcess->setProcessEnvironment(env);
    m_serverProcess->setWorkingDirectory(cwd);

    connect(m_serverProcess, &QProcess::readyReadStandardOutput,
            this, &DSHHub::handleServerOutput);
    connect(m_serverProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &DSHHub::handleServerFinished);

    m_statusLabel->setText(QStringLiteral("正在启动内置 DSH 服务端..."));

    m_serverProcess->start(nodePath, QStringList{
        entryPath, dshEntry, QStringLiteral("web"),
        QStringLiteral("--port"), QStringLiteral("0")
    });
}

void DSHHub::handleServerOutput()
{
    if (!m_serverProcess)
        return;

    while (m_serverProcess->canReadLine()) {
        const QString line = QString::fromUtf8(m_serverProcess->readLine()).trimmed();

        QRegularExpression re(QStringLiteral("dsh web: http://127\\.0\\.0\\.1:(\\d+)"));
        const QRegularExpressionMatch match = re.match(line);

        if (match.hasMatch()) {
            const int port = match.captured(1).toInt();
            qDebug().noquote() << QStringLiteral("[DSH Hub] 服务端端口: %1").arg(port);
            m_api->setBaseUrl(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port)));

            m_api->openStreams();
            m_statusLabel->setText(QStringLiteral("DSH 服务端已启动 (%1)").arg(port));
            return;
        }

        // 把服务端错误信息也显示出来，方便定位启动失败原因
        if (line.contains(QStringLiteral("error"), Qt::CaseInsensitive)
            || line.contains(QStringLiteral("failed"), Qt::CaseInsensitive)
            || line.contains(QStringLiteral("uncaught"), Qt::CaseInsensitive)
            || line.contains(QStringLiteral("exception"), Qt::CaseInsensitive)) {
            m_messages->addSystemMessage(
                QStringLiteral("DSH 服务端: %1").arg(line),
                m_messagesLayout);
        }
    }
}

void DSHHub::handleServerFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitStatus)

    if (m_api && !m_api->isConnected()) {
        m_statusLabel->setText(QStringLiteral("DSH 服务端已退出"));
        m_messages->addSystemMessage(
            QStringLiteral("DSH 服务端已退出，代码: %1").arg(exitCode),
            m_messagesLayout);
    }
}

void DSHHub::onSendClicked()
{
    const QString text = m_chatInput->text().trimmed();
    if (text.isEmpty())
        return;

    if (m_sessionId.isEmpty()) {
        m_messages->addSystemMessage(QStringLiteral("还没有可用会话，正在自动创建..."),
                                     m_messagesLayout);
        ensureSession();
        return;
    }

    // 本地发送时创建一条用户消息，加入消息列表并显示在滚动区
    m_streaming = false;
    if (m_streamTimer)
        m_streamTimer->stop();
    m_messages->addUserMessage(text, m_messagesLayout);

    QJsonObject payload;
    payload.insert(QStringLiteral("sessionId"), m_sessionId);
    payload.insert(QStringLiteral("mode"), QStringLiteral("queue"));

    QJsonArray content;
    QJsonObject textPart;
    textPart.insert(QStringLiteral("type"), QStringLiteral("text"));
    textPart.insert(QStringLiteral("text"), text);
    content.append(textPart);
    payload.insert(QStringLiteral("content"), content);

    m_api->callMethod(
        QStringLiteral("session.prompt"),
        payload,
        [this](const QJsonObject &value) {
            Q_UNUSED(value)
        },
        [this](const DshApiClient::RpcError &error) {
            m_messages->addSystemMessage(
                QStringLiteral("发送失败: %1 %2").arg(error.code, error.message),
                m_messagesLayout);
        });

    m_chatInput->clear();
}

void DSHHub::onNewWorkspaceClicked()
{
    const QString path = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("选择要加入工作区的目录"));

    if (path.isEmpty())
        return;

    QJsonObject payload;
    payload.insert(QStringLiteral("path"), path);

    m_api->callMethod(
        QStringLiteral("workspace.create"),
        payload,
        [this](const QJsonObject &) {
            callSessionList();
        },
        [this](const DshApiClient::RpcError &error) {
            if (m_messages) {
                m_messages->addSystemMessage(
                    QStringLiteral("新建工作区失败: %1 %2").arg(error.code, error.message),
                    m_messagesLayout);
            }
        });
}

void DSHHub::onCreateSessionInWorkspace(const QString &workspaceId)
{
    QJsonObject payload;
    if (!workspaceId.isEmpty())
        payload.insert(QStringLiteral("workspaceId"), workspaceId);

    m_api->callMethod(
        QStringLiteral("session.create"),
        payload,
        [this, workspaceId](const QJsonObject &value) {
            const QString newSessionId = value.value(QStringLiteral("sessionId")).toString();
            if (newSessionId.isEmpty())
                return;

            m_sessionId = newSessionId;
            if (m_topBar)
                m_topBar->setTitle(QStringLiteral("未命名会话"));
            if (m_sessionList) {
                m_sessionList->addSessionToWorkspace(newSessionId, QStringLiteral("未命名会话"), workspaceId);
                m_sessionList->setCurrentSession(newSessionId);
            }

            m_history.reset();
            if (m_loadMoreButton)
                m_loadMoreButton->hide();
            loadHistory();
        },
        [this](const DshApiClient::RpcError &error) {
            if (m_messages) {
                m_messages->addSystemMessage(
                    QStringLiteral("新建会话失败: %1 %2").arg(error.code, error.message),
                    m_messagesLayout);
            }
        });
}


void DSHHub::cacheCurrentMessages()
{
    m_cacheManager.cacheOrDiscardCurrentSession(m_sessionId, m_messages, m_messagesLayout);
    m_messages = nullptr;
}

bool DSHHub::tryRestoreCachedMessages(const QString &sessionId)
{
    m_messages = m_cacheManager.restoreCachedSession(sessionId, m_messagesLayout);
    if (!m_messages)
        return false;

    hideLoadingIndicator();
    if (m_loadMoreButton)
        m_loadMoreButton->hide();

    scrollToBottomNow();

    return true;
}

MessageQuery *DSHHub::buildOffscreenQuery(const QJsonArray &events)
{
    auto *holder = new QWidget;
    auto *layout = new QVBoxLayout(holder);

    auto *query = new MessageQuery;
    appendHistoryEventsToQuery(query, layout, events);

    // 从临时离屏布局中摘下，并解除父子关系，方便后续 attach 到真实消息区
    query->detachFromLayout(layout);
    query->releaseWidgets();
    delete holder;

    return query;
}

void DSHHub::swapToMessageQuery(MessageQuery *query)
{
    if (!m_scrollArea)
        return;

    m_scrollArea->setUpdatesEnabled(false);

    if (m_messages) {
        m_messages->clear();
        delete m_messages;
        m_messages = nullptr;
    }

    m_messages = query;
    m_messages->attachToLayout(m_messagesLayout);

    // 在刷新前同步滚到底部，避免先显示顶部再闪烁
    scrollToBottomNow();
}

void DSHHub::scrollToBottomNow()
{
    if (!m_scrollArea || !m_scrollArea->widget())
        return;

    // 避免流式输出时频繁触发多个滚动任务
    if (m_scrollToBottomScheduled)
        return;
    m_scrollToBottomScheduled = true;

    // 在布局完成前一直禁用刷新，等滚动到底部后再一次性显示，
    // 避免先看到顶部再闪到底部。
    m_scrollArea->setUpdatesEnabled(false);

    // 强制触发一次布局，让 scrollbar maximum 尽快有效
    if (m_scrollArea->widget()->layout())
        m_scrollArea->widget()->layout()->activate();

    // 等 Qt 完成本轮布局/事件处理后，再真正滚动并恢复刷新
    QTimer::singleShot(0, this, [this]() {
        if (!m_scrollArea)
            return;

        // 再次强制布局/调整尺寸，确保 scrollbar maximum 已经更新
        QWidget *content = m_scrollArea->widget();
        if (content) {
            if (content->layout())
                content->layout()->activate();
            content->adjustSize();
        }

        if (m_scrollArea->verticalScrollBar())
            m_scrollArea->verticalScrollBar()->setValue(
                m_scrollArea->verticalScrollBar()->maximum());

        m_scrollToBottomScheduled = false;
        m_scrollArea->setUpdatesEnabled(true);
        m_scrollArea->viewport()->update();
    });
}

void DSHHub::onHistoryPrefetched(const QString &sessionId, const QJsonArray &events)
{
    if (sessionId.isEmpty() || events.isEmpty())
        return;

    // 当前会话正在正式加载，不覆盖；已有控件缓存也不需要
    if (sessionId == m_sessionId || m_cacheManager.hasCachedMessages(sessionId))
        return;

    m_cacheManager.storePrefetchedHistory(sessionId, events);
}

void DSHHub::onSessionSelected(const QString &sessionId)
{
    if (sessionId.isEmpty())
        return;

    hideLoadingIndicator();

    // 先把当前会话的消息控件缓存起来
    cacheCurrentMessages();

    // 再切换到目标会话
    m_sessionId = sessionId;

    if (m_sessionList)
        m_sessionList->setCurrentSession(sessionId);

    if (m_topBar && m_sessionList)
        m_topBar->setTitle(m_sessionList->titleForSession(sessionId));

    // 如果目标会话已经有缓存控件，直接贴回去，不再重新加载/渲染
    if (tryRestoreCachedMessages(sessionId))
        return;

    // 没有控件缓存：创建新的消息容器
    m_messages = new MessageQuery;

    m_history.setLimit(20);
    m_history.setHasMore(false);

    // 如果后台已经预取过最近几条历史，先立刻显示，再继续向上加载更多
    const QJsonArray prefetched = m_cacheManager.takePrefetchedHistory(sessionId);
    m_usingPrefetched = !prefetched.isEmpty();
    if (!prefetched.isEmpty()) {
        m_history.setEventCount(prefetched.size());
        renderHistoryEvents(prefetched, false);
        // 先显示最近几条时也直接滚到底部
        scrollToBottomNow();

    } else {
        m_history.setEventCount(0);
    }

    if (m_loadMoreButton)
        m_loadMoreButton->hide();
    if (prefetched.isEmpty())
        showLoadingIndicator();
    loadHistory();

}

void DSHHub::showLoadingIndicator()
{
    hideLoadingIndicator();

    if (!m_messagesLayout)
        return;

    // 用容器 + 上下伸缩项实现垂直居中
    auto *container = new QWidget;
    auto *box = new QVBoxLayout(container);
    box->setContentsMargins(0, 0, 0, 0);
    box->addStretch(1);

    auto *loading = new LoadingCard(container);

    box->addWidget(loading, 0, Qt::AlignHCenter);
    box->addStretch(1);

    m_messagesLayout->addWidget(container, 1);
    m_loadingContainer = container;
    m_loadingCard = loading;
}

void DSHHub::hideLoadingIndicator()
{
    if (!m_loadingContainer && !m_loadingCard)
        return;

    if (m_messagesLayout && m_loadingContainer) {
        m_messagesLayout->removeWidget(m_loadingContainer);
        m_loadingContainer->deleteLater();
    } else if (m_messagesLayout && m_loadingCard) {
        m_messagesLayout->removeWidget(m_loadingCard);
        m_loadingCard->deleteLater();
    }

    m_loadingContainer = nullptr;
    m_loadingCard = nullptr;
}

void DSHHub::onClearConversationClicked()
{
    m_sessionList->clearAllSessions(
        m_dshHome,
        [this]() {
            hideLoadingIndicator();
            if (m_messages)
                m_messages->clear();
            m_cacheManager.clearAll();
            m_cacheManager.clearPrefetchedHistory();
            m_sessionId.clear();

            if (m_messages) {
                if (AgentMessageUnit *agent = m_messages->lastAgentUnit())
                    agent->clearStreamSegments();
            }
        },
        [this]() {
            callSessionCreate();
        });
}

void DSHHub::handleConnected()
{
    m_statusLabel->setText(QStringLiteral("已连接 DSH"));

    ensureSession();
}

void DSHHub::handleDisconnected()
{
    m_statusLabel->setText(QStringLiteral("DSH 连接已断开"));

}

void DSHHub::handleMuxFrame(const QJsonObject &frame)
{
    const QJsonObject payload = frame.value(QStringLiteral("payload")).toObject();
    const QString type = payload.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("session/event")) {
        const QJsonObject event = payload.value(QStringLiteral("event")).toObject();
        const QString eventType = event.value(QStringLiteral("type")).toString();

        if (eventType == QStringLiteral("assistant/message")) {
            const QString thinking = extractThinking(event);


            const QString reply = extractReply(event);

            if (m_streaming) {
                // 已经通过 assistant/chunk 流式显示过，最终消息不再重复追加
                m_streaming = false;
                if (m_streamTimer)
                    m_streamTimer->stop();
                if (AgentMessageUnit *target = m_messages->lastAgentUnitIfLast())
                    target->flushStream();
            } else if (!thinking.isEmpty() || !reply.isEmpty()) {
                // 如果上一条仍然是 Agent 消息，就继续追加到同一个气泡里，保持连续
                AgentMessageUnit *target = m_messages->lastAgentUnitIfLast();
                if (target) {
                    if (!thinking.isEmpty()) {
                        target->appendThinking(thinking);
                    }
                    if (!reply.isEmpty())
                        target->appendMarkdownWithCodeShadow(reply);
                } else {
                    // 否则创建新的 AgentMessageUnit
                    m_messages->addAgentMessage(reply, m_messagesLayout, thinking);
                }
            }
                // 一次对话完成后，刷新会话标题（如果服务端已经生成了标题）
                if (m_sessionList && m_api)
                    m_sessionList->refreshTitles(m_api);
        } else if (eventType == QStringLiteral("assistant/chunk")) {
            const QString chunk = extractEventText(event);

            {
                const QString chunkType = extractChunkType(event);
                const QJsonObject chunkObj = event.value(QStringLiteral("data")).toObject()
                                                 .value(QStringLiteral("chunk")).toObject();
                const QString blockType = chunkObj.value(QStringLiteral("blockType")).toString();


                AgentMessageUnit *streamTarget = m_messages->lastAgentUnitIfLast();
                if (!streamTarget)
                    streamTarget = m_messages->addAgentMessage(QString(), m_messagesLayout);

                if (chunkType == QStringLiteral("reasoning-delta") && !chunk.isEmpty()) {
                    streamTarget->appendStreamChunk(StreamSegment::Thinking, chunk);
                } else if (chunkType == QStringLiteral("text-delta") && !chunk.isEmpty()) {
                    streamTarget->appendStreamChunk(StreamSegment::Reply, chunk);
                }
                // 节流 50ms 批量全量重渲染一次，保证 Markdown/HTML 即时显示
                if (m_streamTimer && !m_streamTimer->isActive())
                    m_streamTimer->start();

                m_streaming = true;
            }
        } else if (eventType == QStringLiteral("user/message")) {
            // 用户消息已在 onSendClicked 中创建 UserMessageUnit，这里避免重复显示。
            // 如果以后需要展示其它端发来的用户消息，可以改成：
            // const QString text = extractEventText(event);
            // if (!text.isEmpty())
            //     m_messages->addUserMessage(text, m_messagesLayout);
        } else if (eventType == QStringLiteral("tool/call")) {
            const ToolCallInfo tool = extractToolCall(event);
            if (tool.valid) {
                AgentMessageUnit *target = m_messages->lastAgentUnit();
                if (!target)
                    target = m_messages->addAgentMessage(QString(), m_messagesLayout);
                target->appendHtml(QStringLiteral("<p style='color:#2a7ab0;'><b>工具调用：</b>%1</p>")
                                       .arg(tool.name.toHtmlEscaped()));
                target->appendHtml(QStringLiteral("<pre>%1</pre>")
                                       .arg(QString::fromUtf8(
                                           QJsonDocument(tool.arguments).toJson(QJsonDocument::Indented))
                                                .toHtmlEscaped()));
                const QString toolHtml =
                    QStringLiteral("<p style='color:#2a7ab0;'><b>工具调用：</b>%1</p>")
                        .arg(tool.name.toHtmlEscaped())
                    + QStringLiteral("<pre>%1</pre>")
                        .arg(QString::fromUtf8(
                            QJsonDocument(tool.arguments).toJson(QJsonDocument::Indented))
                                 .toHtmlEscaped());
                target->appendStreamChunk(StreamSegment::ToolCall, toolHtml);
            }
        } else if (eventType == QStringLiteral("tool/result")) {
            const ToolResultInfo result = extractToolResult(event);
                const QString resultHtml =
                    QStringLiteral("<p style='color:#2a7ab0;'><b>工具结果：</b></p>")
                    + QStringLiteral("<pre>%1</pre>").arg(result.message.toHtmlEscaped());
                if (AgentMessageUnit *resultTarget = m_messages->lastAgentUnit())
                    resultTarget->appendStreamChunk(StreamSegment::ToolResult, resultHtml);
            if (result.valid) {
                AgentMessageUnit *target = m_messages->lastAgentUnit();
                if (!target)
                    target = m_messages->addAgentMessage(QString(), m_messagesLayout);
                target->appendHtml(QStringLiteral("<p style='color:#2a7ab0;'><b>工具结果：</b></p>"));
                target->appendHtml(QStringLiteral("<pre>%1</pre>")
                                       .arg(result.message.toHtmlEscaped())); // RESULT_ARG
            }
        }
    } else if (type == QStringLiteral("question/requested")) {
        const QList<QuestionInfo> questions = extractQuestions(payload);
        QString text = QStringLiteral("DSH 提问：");
        for (const QuestionInfo &question : questions) {
            text += QStringLiteral("\n• %1").arg(question.question);
        }
        m_messages->addSystemMessage(text, m_messagesLayout);
    } else if (type == QStringLiteral("approval/requested")) {
        const ApprovalInfo approval = extractApproval(payload);
        m_messages->addSystemMessage(
            QStringLiteral("需要审批：%1").arg(approval.toolName),
            m_messagesLayout);

        QJsonObject answer;
        answer.insert(QStringLiteral("sessionId"), approval.sessionId);
        answer.insert(QStringLiteral("approvalId"), approval.approvalId);
        answer.insert(QStringLiteral("outcome"), QStringLiteral("allowed-once"));
        m_api->respond(frame.value(QStringLiteral("rpcId")).toString(), answer);
    }
}


void DSHHub::handleHostFrame(const QJsonObject &frame)
{
    Q_UNUSED(frame)
}

void DSHHub::handleTransportError(const QString &context, const QString &message)
{
    m_messages->addSystemMessage(
        QStringLiteral("传输错误 [%1]: %2").arg(context, message),
        m_messagesLayout);
}


void DSHHub::showNoMoreToast()
{
    if (!m_toastLabel)
        return;

    QWidget *parent = qobject_cast<QWidget *>(m_toastLabel->parent());
    if (!parent)
        parent = this;

    m_toastLabel->setText(QStringLiteral("啊哦，没有更多了"));
    m_toastLabel->adjustSize();
    m_toastLabel->setGeometry(
        (parent->width() - m_toastLabel->width() - 32) / 2,
        parent->height() - m_toastLabel->height() - 24,
        m_toastLabel->width() + 32,
        m_toastLabel->height());
    m_toastLabel->show();
    m_toastLabel->raise();

    auto *effect = new QGraphicsOpacityEffect(m_toastLabel);
    m_toastLabel->setGraphicsEffect(effect);

    auto *fadeIn = new QPropertyAnimation(effect, "opacity", m_toastLabel);
    fadeIn->setDuration(180);
    fadeIn->setStartValue(0.0);
    fadeIn->setEndValue(1.0);
    connect(fadeIn, &QPropertyAnimation::finished, this, [this]() {
        QTimer::singleShot(1200, this, [this]() {
            if (!m_toastLabel)
                return;

            auto *effect = qobject_cast<QGraphicsOpacityEffect *>(m_toastLabel->graphicsEffect());
            if (!effect)
                return;

            auto *fadeOut = new QPropertyAnimation(effect, "opacity", m_toastLabel);
            fadeOut->setDuration(300);
            fadeOut->setStartValue(1.0);
            fadeOut->setEndValue(0.0);
            connect(fadeOut, &QPropertyAnimation::finished, m_toastLabel, [this]() {
                if (m_toastLabel)
                    m_toastLabel->hide();
            });
            fadeOut->start(QAbstractAnimation::DeleteWhenStopped);
        });
    });
    fadeIn->start(QAbstractAnimation::DeleteWhenStopped);
}

void DSHHub::onLoadMoreHistory()
{
    if (m_sessionId.isEmpty() || m_history.isLoading())
        return;

    m_history.setLoadMoreRequested(true);
    m_history.increaseLimit(20);
    loadHistory();
}

void DSHHub::loadHistory()
{
    if (m_sessionId.isEmpty() || m_history.isLoading())
        return;

    const QString requestedSessionId = m_sessionId;
    m_history.setLoading(true);

    QJsonObject payload;
    payload.insert(QStringLiteral("sessionId"), m_sessionId);
    payload.insert(QStringLiteral("maxMessages"), m_history.limit());

    const int oldScroll = m_scrollArea ? m_scrollArea->verticalScrollBar()->value() : 0;
    const int oldScrollMax = m_scrollArea ? m_scrollArea->verticalScrollBar()->maximum() : 0;
    const int oldCount = m_history.eventCount();

    m_api->callMethod(
        QStringLiteral("session.history"),
        payload,
        [this, oldScroll, oldScrollMax, oldCount, requestedSessionId](const QJsonObject &value) {
            m_history.setLoading(false);

            if (m_sessionId != requestedSessionId) {
                // 用户已经切换到其它会话，丢弃这次过期响应
                loadHistory();
                return;
            }

            const QJsonArray events = value.value(QStringLiteral("events")).toArray();
            const int newCount = events.size();

            if (oldCount == 0) {
                // 首次加载：显示底部若干条，并滚动到底部
                m_history.setEventCount(newCount);
                m_history.setHasMore(newCount >= m_history.limit());
                // 离屏构建完整历史消息，再一次性切换，避免闪烁
                hideLoadingIndicator();
                if (m_loadMoreButton)
                    m_loadMoreButton->hide();
                swapToMessageQuery(buildOffscreenQuery(events));
                finishInitialization();
            } else if (newCount > oldCount) {
                m_history.setEventCount(newCount);
                m_history.setHasMore(newCount >= m_history.limit());

                if (m_usingPrefetched) {
                    // 之前先显示了预取的最近几条，现在离屏构建完整历史后一次性切换，
                    // 避免边界处思考链拆开，也减少闪烁。
                    m_usingPrefetched = false;
                    swapToMessageQuery(buildOffscreenQuery(events));
                    finishInitialization();
                } else {
                    // 加载更多：把新增的旧消息插入到当前消息上方
                    QJsonArray older;
                    for (int i = 0; i < newCount - oldCount; ++i)
                        older.append(events.at(i));

                    renderHistoryEvents(older, true);

                    // 保持当前可视位置不跳动
                    if (m_scrollArea) {
                        QTimer::singleShot(0, this, [this, oldScroll, oldScrollMax]() {
                            if (m_scrollArea && m_scrollArea->verticalScrollBar()) {
                                QScrollBar *bar = m_scrollArea->verticalScrollBar();
                                const int delta = bar->maximum() - oldScrollMax;
                                bar->setValue(oldScroll + delta);
                            }
                        });
                    }
                }
            } else {
                m_history.setHasMore(false);
            }

            if (m_history.loadMoreRequested() && !m_history.hasMore())
                showNoMoreToast();
            m_history.setLoadMoreRequested(false);

            if (m_loadMoreButton)
                m_loadMoreButton->setVisible(true);
        },
        [this, requestedSessionId](const DshApiClient::RpcError &error) {
            m_history.setLoading(false);
            m_usingPrefetched = false;
            finishInitialization();



            if (m_sessionId != requestedSessionId) {
                // 切换到了新会话，继续加载最新选中的会话
                loadHistory();
            } else if (m_messages) {
                hideLoadingIndicator();

                m_messages->clear();
                m_messages->addSystemMessage(
                    QStringLiteral("加载会话失败: %1 %2").arg(error.code, error.message),
                    m_messagesLayout);
            }
        });
}

void DSHHub::renderHistoryEvents(const QJsonArray &events, bool prepend)
{

    if (!m_messages || !m_messagesLayout)
        return;

    const int layoutIndex = 1; // 0 是“加载更多”按钮

    if (!prepend) {
        appendHistoryEventsToQuery(m_messages, m_messagesLayout, events);
        return;
    }

    

    // 向前插入时，先把连续的 assistant/message 合并成同一个气泡，避免思考链被拆散
    struct PendingItem
    {
        int type = 0; // 0: user, 1: agent
        QString text;
        QString thinking;
    };

    QList<PendingItem> items;
    for (const auto &value : events) {
        QJsonObject event = value.toObject();
        if (event.contains(QStringLiteral("event")) && event.value(QStringLiteral("event")).isObject())
            event = event.value(QStringLiteral("event")).toObject();

        const QString type = event.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("user/message")) {
            const QString text = extractEventText(event).trimmed();
            if (!text.isEmpty())
                items.append({0, text, QString()});
        } else if (type == QStringLiteral("assistant/message")) {
            const QString thinking = extractThinking(event);
            const QString reply = extractReply(event);
            if (reply.isEmpty() && thinking.isEmpty())
                continue;

            if (!items.isEmpty() && items.last().type == 1) {
                if (!items.last().text.isEmpty() && !reply.isEmpty())
                    items.last().text += QStringLiteral("\n\n");
                items.last().text += reply;
                if (!thinking.isEmpty()) {
                    if (!items.last().thinking.isEmpty())
                        items.last().thinking += QStringLiteral("\n\n");
                    items.last().thinking += thinking;
                }
            } else {
                items.append({1, reply, thinking});
            }
        }
    }

    // 倒序插入，保证最终顺序仍然是 旧 -> 新
    for (int i = items.size() - 1; i >= 0; --i) {
        if (items.at(i).type == 0) {
            m_messages->insertUserMessage(items.at(i).text, m_messagesLayout, layoutIndex);
        } else {
            m_messages->insertAgentMessage(items.at(i).text, m_messagesLayout,
                                           layoutIndex, items.at(i).thinking);
        }
    }
}

void DSHHub::ensureSession()
{
    callSessionList();
}

void DSHHub::callSessionList()
{
    m_api->callMethod(
        QStringLiteral("workspace.list"),
        {},
        [this](const QJsonObject &value) {
            if (m_sessionList)
                m_sessionList->setWorkspaces(value.value(QStringLiteral("items")).toArray());
            loadSessionList();
        },
        [this](const DshApiClient::RpcError &) {
            if (m_sessionList)
                m_sessionList->setWorkspaces(QJsonArray());
            loadSessionList();
        });
}

void DSHHub::loadSessionList()
{
    m_api->callMethod(
        QStringLiteral("session.list"),
        {},
        [this](const QJsonObject &value) {
            const QJsonArray items = value.value(QStringLiteral("items")).toArray();
            bool autoSelected = false;

            for (const auto &item : items) {
                const QJsonObject session = item.toObject();
                const QString origin = session.value(QStringLiteral("origin")).toString();
                const bool isSubagent = origin == QStringLiteral("subagent")
                                        || session.contains(QStringLiteral("parentSessionId"));
                const bool running = session.value(QStringLiteral("running")).toBool();
                if (isSubagent)
                    continue;

                const QString sid = session.value(QStringLiteral("sessionId")).toString();
                if (sid.isEmpty())
                    continue;

                // 原版 DSH 的标题在 session.projections.values 里
                const QJsonObject projections = session.value(QStringLiteral("projections")).toObject();
                const QJsonObject projectionValues = projections.value(QStringLiteral("values")).toObject();
                QString label = projectionValues.value(QStringLiteral("title")).toString();
                if (label.isEmpty())
                    label = projectionValues.value(QStringLiteral("sessionTitle")).toString();
                if (label.isEmpty())
                    label = projectionValues.value(QStringLiteral("session.title")).toString();
                if (label.isEmpty())
                    label = QStringLiteral("未命名会话");

                if (m_sessionList)
                    m_sessionList->addSession(sid, label);
                // 后台预取每个会话最近 3 条历史，打开时可以先快速显示
                if (m_prefetcher && m_api)
                    m_prefetcher->prefetchHistory(m_api->baseUrl(), sid, 3);


                if (!isSubagent && !running) {
                    if (autoSelected) continue;
                    m_sessionId = sid;
                    if (m_topBar)
                        m_topBar->setTitle(label);
                    m_history.setLimit(20);
                    m_history.setEventCount(0);
                    m_history.setHasMore(false);
                    hideLoadingIndicator();
                    if (m_messages)
                        m_messages->clear();
                    if (m_loadMoreButton)
                        m_loadMoreButton->hide();
                    loadHistory();
                    if (m_sessionList)
                        m_sessionList->setCurrentSession(m_sessionId);

                    autoSelected = true;
                }
            }

            if (m_sessionId.isEmpty()) {
                callSessionCreate();
            }
        },
        [this](const DshApiClient::RpcError &error) {
            finishInitialization();
            m_messages->addSystemMessage(
                QStringLiteral("获取会话列表失败: %1 %2").arg(error.code, error.message),
                m_messagesLayout);
        });
}

void DSHHub::callSessionCreate()
{
    m_api->callMethod(
        QStringLiteral("session.create"),
        {},
        [this](const QJsonObject &value) {
            m_sessionId = value.value(QStringLiteral("sessionId")).toString();
            if (m_topBar)
                m_topBar->setTitle(QStringLiteral("未命名会话"));
            if (m_sessionList) {
                m_sessionList->addSession(m_sessionId, QStringLiteral("未命名会话"));
                m_sessionList->setCurrentSession(m_sessionId);
            }
            m_history.setLimit(20);
            m_history.setEventCount(0);
            m_history.setHasMore(false);
            if (m_messages)
                m_messages->clear();
            if (m_loadMoreButton)
                m_loadMoreButton->hide();
            loadHistory();
        },
        [this](const DshApiClient::RpcError &error) {
            finishInitialization();
            m_messages->addSystemMessage(
                QStringLiteral("创建会话失败: %1 %2").arg(error.code, error.message),
                m_messagesLayout);
        });
}
