#include "DSHHub.h"
#include "ThemeManager.h"
#include "ChatInputWidget.h"
#include "Sidebar.h"

#include "DshApiClient.h"
#include "SessionPrefetcher.h"
#include "CodeHighlighter.h"
#include "SpinnerWidget.h"
#include "TopBar.h"
#include "Settings.h"
#include "PluginsPopup.h"


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
        } else if (type == QStringLiteral("tool/call")) {
            const ToolCallInfo tool = extractToolCall(event);
            if (tool.valid) {
                AgentMessageUnit *target = query->lastAgentUnitIfLast();
                if (!target)
                    target = query->addAgentMessage(QString(), layout);
                const QString html = QStringLiteral("<pre>%1</pre>")
                    .arg(QString::fromUtf8(
                             QJsonDocument(tool.arguments).toJson(QJsonDocument::Indented))
                              .toHtmlEscaped());
                target->appendToolCall(tool.name, html);
            }
        } else if (type == QStringLiteral("tool/result")) {
            const ToolResultInfo result = extractToolResult(event);
            if (result.valid) {
                AgentMessageUnit *target = query->lastAgentUnitIfLast();
                if (!target)
                    target = query->addAgentMessage(QString(), layout);
                const QString html = QStringLiteral("<pre>%1</pre>").arg(result.message.toHtmlEscaped());
                target->appendToolResult(html);
            }
        }
    }
}
}

DSHHub::DSHHub(QWidget *parent)
    : QMainWindow(parent)
    , m_api(new DshApiClient(this))
{
    qInfo().noquote() << QStringLiteral("[DSH Hub] constructor started");
    setWindowTitle(QStringLiteral("DSH Hub"));

    // 创建界面
    auto *central = new QWidget(this);
    central->setStyleSheet(QStringLiteral("QWidget {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("windowBg")) + QStringLiteral(";") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QLineEdit {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border: 1px solid ") + QStringLiteral("#D0D7DE") + QStringLiteral(";") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("  padding: 6px 10px;") + QStringLiteral("}") + QStringLiteral("QLineEdit:focus {") + QStringLiteral("  border-color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QPushButton {") + QStringLiteral("  background: ") + QStringLiteral("#E8EAED") + QStringLiteral(";") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("  padding: 6px 14px;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QPushButton:hover {") + QStringLiteral("  background: ") + QStringLiteral("#DDE0E4") + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QPushButton:pressed {") + QStringLiteral("  background: ") + QStringLiteral("#CDD0D5") + QStringLiteral(";") + QStringLiteral("}"));
    auto *layout = new QVBoxLayout(central);

    m_statusLabel = new QLabel(QStringLiteral("正在连接 DSH..."), central);
    m_statusLabel->hide(); // 不再显示顶部状态文字，仅保留变量避免改动其他逻辑

    m_scrollArea = new QScrollArea(central);
    m_scrollArea->setFixedWidth(900);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setStyleSheet(QStringLiteral("QScrollArea {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("windowBg")) + QStringLiteral(";") + QStringLiteral("  border: none;") + QStringLiteral("}") + QStringLiteral("QScrollArea QScrollBar:vertical {") + QStringLiteral("  background: transparent;") + QStringLiteral("  width: 8px;") + QStringLiteral("  margin: 2px;") + QStringLiteral("}") + QStringLiteral("QScrollArea QScrollBar::handle:vertical {") + QStringLiteral("  background: ") + QStringLiteral("#C1C7CF") + QStringLiteral(";") + QStringLiteral("  border-radius: 4px;") + QStringLiteral("  min-height: 30px;") + QStringLiteral("}") + QStringLiteral("QScrollArea QScrollBar::handle:vertical:hover {") + QStringLiteral("  background: ") + QStringLiteral("#A8B0B9") + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QScrollArea QScrollBar::add-line:vertical,") + QStringLiteral("QScrollArea QScrollBar::sub-line:vertical {") + QStringLiteral("  height: 0;") + QStringLiteral("}") + QStringLiteral("QScrollArea QScrollBar::add-page:vertical,") + QStringLiteral("QScrollArea QScrollBar::sub-page:vertical {") + QStringLiteral("  background: transparent;") + QStringLiteral("}"));

    auto *scrollContent = new QWidget;
    scrollContent->setStyleSheet(QStringLiteral("background: ") + Theme::color(QStringLiteral("windowBg")) + QStringLiteral(";"));
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
    rightPanel->setStyleSheet(QStringLiteral("background: ") + Theme::color(QStringLiteral("windowBg")) + QStringLiteral(";") + QStringLiteral("border-radius: 12px;"));

    m_chatInput = new ChatInputWidget(rightPanel);

    // 底部“没有更多了”提示
    m_toastLabel = new QLabel(QStringLiteral("啊哦，没有更多了"), this);
    m_toastLabel->setStyleSheet(QStringLiteral("QLabel {") + QStringLiteral("  background: rgba(31,35,40,0.85);") + QStringLiteral("  color: white;") + QStringLiteral("  border-radius: 16px;") + QStringLiteral("  padding: 8px 16px;") + QStringLiteral("  font-size: 13px;") + QStringLiteral("}"));
    m_toastLabel->setAlignment(Qt::AlignCenter);
    m_toastLabel->hide();


    auto *panelLayout = new QVBoxLayout(rightPanel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    // 左侧灰色会话列表
    m_sidebar = new Sidebar(central);
    m_sidebar->setFixedWidth(240);

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

    bodyLayout->addWidget(m_sidebar);
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
    m_initOverlay->setStyleSheet(QStringLiteral("QWidget#initOverlay {") + QStringLiteral("  background-color: rgba(128,128,128,0.65);") + QStringLiteral("}"));
    auto *overlayLayout = new QVBoxLayout(m_initOverlay);

    // 现代化横版卡片：宽高比约 5:3
    auto *initCard = new QWidget(m_initOverlay);
    initCard->setFixedSize(400, 240);
    initCard->setStyleSheet(QStringLiteral("QWidget {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border-radius: 20px;") + QStringLiteral("}"));

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
    m_initLabel->setStyleSheet(QStringLiteral("QLabel {") + QStringLiteral("  background: transparent;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("  font-size: 18px;") + QStringLiteral("  font-weight: 600;") + QStringLiteral("}"));
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




    connect(m_sidebar, &Sidebar::newWorkspaceRequested,
            this, &DSHHub::onNewWorkspaceClicked);
    connect(m_sidebar, &Sidebar::createSessionInWorkspaceRequested,
            this, &DSHHub::onCreateSessionInWorkspace);
    connect(m_sidebar, &Sidebar::sessionSelected,
            this, &DSHHub::onSessionSelected);
    connect(m_sidebar, &Sidebar::clearRequested,
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
    connect(m_sidebar, &Sidebar::settingsRequested,
            this, &DSHHub::openSettings);
    connect(m_sidebar, &Sidebar::pluginsRequested,
            this, &DSHHub::openPlugins);
    connect(m_sidebar, &Sidebar::themeToggleRequested,
            this, &DSHHub::toggleTheme);


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

    if (m_pluginsOverlay)
        m_pluginsOverlay->setGeometry(rect());
}

void DSHHub::finishInitialization()
{
    if (m_initializationComplete)
        return;

    m_initializationComplete = true;
    qInfo().noquote() << QStringLiteral("[DSH Hub] initialization complete");

    if (m_initOverlay) {
        m_initOverlay->hide();
        m_initOverlay->deleteLater();
        m_initOverlay = nullptr;
        m_initLabel = nullptr;
    }

    emit initializationComplete();
}

bool DSHHub::isInitializationComplete() const
{
    return m_initializationComplete;
}

void DSHHub::openSettings()
{
    if (m_settings)
        return;

    // 灰色蒙版，和初始化蒙版一致，禁止主窗口交互
    m_settingsOverlay = new QWidget(this);
    m_settingsOverlay->setObjectName(QStringLiteral("settingsOverlay"));
    m_settingsOverlay->setAttribute(Qt::WA_StyledBackground, true);
    m_settingsOverlay->setStyleSheet(QStringLiteral("QWidget#settingsOverlay {") + QStringLiteral("  background-color: rgba(128,128,128,0.65);") + QStringLiteral("}"));
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

void DSHHub::toggleTheme()
{
    Theme::switchTheme(this);
}

void DSHHub::openPlugins()
{
    if (m_pluginsPopup)
        return;

    m_pluginsOverlay = new QWidget(this);
    m_pluginsOverlay->setObjectName(QStringLiteral("pluginsOverlay"));
    m_pluginsOverlay->setAttribute(Qt::WA_StyledBackground, true);
    m_pluginsOverlay->setStyleSheet(QStringLiteral("QWidget#pluginsOverlay {") + QStringLiteral("  background-color: rgba(128,128,128,0.65);") + QStringLiteral("}"));
    m_pluginsOverlay->setGeometry(rect());
    m_pluginsOverlay->raise();
    m_pluginsOverlay->show();

    m_pluginsPopup = new PluginsPopup(m_api ? m_api->baseUrl() : QUrl(), this);
    m_pluginsPopup->move(geometry().center() - m_pluginsPopup->rect().center());
    m_pluginsPopup->show();

    connect(m_pluginsPopup, &PopupWindow::closed, this, [this]() {
        if (m_pluginsOverlay) {
            m_pluginsOverlay->hide();
            m_pluginsOverlay->deleteLater();
            m_pluginsOverlay = nullptr;
        }
        m_pluginsPopup->deleteLater();
        m_pluginsPopup = nullptr;
    });
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

    qInfo().noquote() << QStringLiteral("[DSH Hub] startBundledServer, dshHome=") << dshHome;
    m_dshHome = dshHome;

    if (!QFile::exists(nodePath) || !QFile::exists(entryPath) || !QFile::exists(dshEntry)) {
        m_statusLabel->setText(QStringLiteral("未找到内置 DSH 服务端，尝试连接 3080..."));
        m_api->setBaseUrl(QUrl(QStringLiteral("http://127.0.0.1:3080")));

        m_api->openStreams();
        return;
    }
    // 使用独立的用户数据目录，避免继承 DSH Desktop 的聊天记录/会话
    QDir().mkpath(cwd);
    QDir().mkpath(dshHome);

    // 如果 profile 里引用的插件包不存在（例如插件市场被删除），自动从 bundles 中移除，
    // 避免 DSH 因为缺少可选插件而无法启动。
    {
        const QString profileDir = dshHome + QStringLiteral("/profiles/web");
        const QString manifestPath = profileDir + QStringLiteral("/package.json");

        if (!QFile::exists(manifestPath)) {
            // profile 不存在时，先创建最小可启动配置，避免 dsh 自动重建出 dsh-web-app
            QDir().mkpath(profileDir);

            QJsonObject root;
            root.insert(QStringLiteral("name"), QStringLiteral("dsh-profile-web"));
            root.insert(QStringLiteral("private"), true);
            root.insert(QStringLiteral("dependencies"), QJsonObject());

            QJsonArray bundles;
            bundles.append(QStringLiteral("@deepseek-ai/dsh-base"));

            const QString marketPkg = profileDir + QStringLiteral("/node_modules/dshmarket/package.json");
            if (QFile::exists(marketPkg))
                bundles.append(QStringLiteral("dshmarket"));

            QJsonObject profile;
            profile.insert(QStringLiteral("bundles"), bundles);

            QJsonObject dsh;
            dsh.insert(QStringLiteral("profile"), profile);

            root.insert(QStringLiteral("dsh"), dsh);

            QFile out(manifestPath);
            if (out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
                out.close();
            }

            // 创建本地 URL 打印插件，DSH Hub 依赖它输出来发现服务端口
            const QString urlPrinterDir = profileDir + QStringLiteral("/node_modules/dsh-url-printer");
            QDir().mkpath(urlPrinterDir);

            QFile urlPkg(urlPrinterDir + QStringLiteral("/package.json"));
            if (urlPkg.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                urlPkg.write("{\n  \"name\": \"dsh-url-printer\",\n  \"version\": \"1.0.0\",\n  \"private\": true,\n  \"type\": \"module\",\n  \"main\": \"index.js\"\n}\n");
                urlPkg.close();
            }

            QFile urlIndex(urlPrinterDir + QStringLiteral("/index.js"));
            if (urlIndex.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                urlIndex.write("import z from \"@deepseek-ai/schemastery\";\n");
                urlIndex.write("const name = \"dsh-url-printer\";\n");
                urlIndex.write("const inject = [\"webServer\"];\n");
                urlIndex.write("const Config = z.object({});\n");
                urlIndex.write("function apply(ctx) {\n");
                urlIndex.write("  const printUrl = () => {\n");
                urlIndex.write("    const port = ctx.webServer?.port;\n");
                urlIndex.write("    if (port !== undefined) console.log(`dsh web: http://127.0.0.1:${port}`);\n");
                urlIndex.write("  };\n");
                urlIndex.write("  const settled = ctx.get(\"loader\")?.await();\n");
                urlIndex.write("  if (settled === undefined) printUrl();\n");
                urlIndex.write("  else settled.then(() => { if (ctx.get(\"webServer\") !== undefined) printUrl(); }, () => {});\n");
                urlIndex.write("}\n");
                urlIndex.write("export { Config, apply, inject, name };\n");
                urlIndex.close();
            }

            QFile patch(profileDir + QStringLiteral("/cordis.patch.yml"));
            if (patch.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                patch.write("# Minimal API-only profile for the DSH Hub Qt client.\n");
                patch.write("- id: hmr\n  disabled: true\n\n");
                patch.write("- insert:\n");
                patch.write("    - id: storage\n      name: '@deepseek-ai/dsh-storage'\n\n");
                patch.write("    - id: storage-json\n      name: '@deepseek-ai/dsh-storage-json'\n      config:\n        root: !!js dshHomePath('storages')\n\n");
                patch.write("    - id: storage-domain\n      name: '@deepseek-ai/dsh-storage-domain'\n      config:\n        backend: json\n\n");
                patch.write("    - id: workspace\n      name: '@deepseek-ai/dsh-workspace'\n\n");
                patch.write("    - id: session-projection-cache\n      name: '@deepseek-ai/dsh-session-projection-cache'\n      config:\n        writeEveryEvents: 200\n        writeIntervalMs: 5000\n\n");
                patch.write("    - id: plugin-inventory\n      name: '@deepseek-ai/dsh-host-plugin-inventory'\n\n");
                patch.write("    - id: api-gateway\n      name: '@deepseek-ai/dsh-host-apiproxy'\n\n");
                patch.write("    - id: cordis-host-runner\n      name: '@deepseek-ai/dsh-cordis-host-runner'\n\n");
                patch.write("    - id: web-startup\n      name: '@deepseek-ai/dsh-web-app/startup'\n\n");
                patch.write("    - id: webserver\n      name: '@deepseek-ai/dsh-host-webserver'\n      inject: [webStartup]\n      config:\n        host: !!js ctx.webStartup.host ?? '127.0.0.1'\n        port: !!js ctx.webStartup.port ?? 3080\n\n");
                patch.write("    - id: url-printer\n      name: 'dsh-url-printer'\n      inject: [webServer]\n\n");
                patch.write("    - id: connection\n      name: '@deepseek-ai/dsh-client-connection'\n      inject: []\n      config:\n        trustedHosts: []\n\n");
                patch.write("    - id: api-remotes\n      name: '@deepseek-ai/dsh-api-remotes'\n\n");
                patch.write("    - id: agent-presets\n      name: '@deepseek-ai/dsh-agent-presets'\n      config:\n        default: standard\n");
                patch.close();
            }

            QFile cordis(profileDir + QStringLiteral("/cordis.yml"));
            if (cordis.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                cordis.write("[]\n");
                cordis.close();
            }

            QFile workspace(profileDir + QStringLiteral("/pnpm-workspace.yaml"));
            if (workspace.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                workspace.write("packages:\n  - .\n\nnodeLinker: hoisted\nautoInstallPeers: false\n");
                workspace.close();
            }
        } else {
            QFile manifestFile(manifestPath);
            if (manifestFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll());
                manifestFile.close();
                if (doc.isObject()) {
                    QJsonObject root = doc.object();
                    QJsonObject dsh = root.value(QStringLiteral("dsh")).toObject();
                    QJsonObject profile = dsh.value(QStringLiteral("profile")).toObject();
                    QJsonArray bundles = profile.value(QStringLiteral("bundles")).toArray();
                    QJsonArray validBundles;
                    QJsonObject dependencies = root.value(QStringLiteral("dependencies")).toObject();
                    bool changed = false;

                    for (const auto &value : bundles) {
                        const QString name = value.toString();

                        if (name == QStringLiteral("@deepseek-ai/dsh-web-app")) {
                            changed = true;
                            dependencies.remove(name);
                            continue;
                        }

                        const QString profilePkg = profileDir + QStringLiteral("/node_modules/") + name + QStringLiteral("/package.json");
                        const QString serverPkg = appDir + QStringLiteral("/resources/server/node_modules/") + name + QStringLiteral("/package.json");
                        if (QFile::exists(profilePkg) || QFile::exists(serverPkg)) {
                            validBundles.append(value);
                        } else {
                            changed = true;
                            dependencies.remove(name);
                        }
                    }

                    if (changed) {
                        profile.insert(QStringLiteral("bundles"), validBundles);
                        dsh.insert(QStringLiteral("profile"), profile);
                        root.insert(QStringLiteral("dsh"), dsh);
                        root.insert(QStringLiteral("dependencies"), dependencies);

                        QFile out(manifestPath);
                        if (out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                            out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
                            out.close();
                        }
                    }
                }
            }
        }
    }

    launchBundledServer(nodePath, entryPath, dshEntry, cwd, dshHome);
}

void DSHHub::launchBundledServer(const QString &nodePath,
                                 const QString &entryPath,
                                 const QString &dshEntry,
                                 const QString &cwd,
                                 const QString &dshHome)
{
    // 如果 3080 已经被占用，说明可能已有 DSH 服务在运行，直接连接，避免 EADDRINUSE
    {
        QTcpSocket probe;
        probe.connectToHost(QStringLiteral("127.0.0.1"), 3080);
        if (probe.waitForConnected(500)) {
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
    qInfo().noquote() << QStringLiteral("[DSH Hub] bundled server process started");
}

void DSHHub::handleServerOutput()
{
    if (!m_serverProcess)
        return;

    while (m_serverProcess->canReadLine()) {
        const QString line = QString::fromUtf8(m_serverProcess->readLine()).trimmed();

        qInfo().noquote() << QStringLiteral("[server]") << line;

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

    qInfo().noquote() << QStringLiteral("[DSH Hub] server finished, code=") << exitCode;

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
        [this](const QJsonObject &) {
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
            if (m_sidebar) {
                m_sidebar->workspaceList()->addSessionToWorkspace(newSessionId, QStringLiteral("未命名会话"), workspaceId);
                m_sidebar->workspaceList()->setCurrentSession(newSessionId);
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
    const bool partial = m_cacheManager.isPartialCache(sessionId);
    m_messages = m_cacheManager.restoreCachedSession(sessionId, m_messagesLayout);
    if (!m_messages)
        return false;

    hideLoadingIndicator();
    if (m_loadMoreButton)
        m_loadMoreButton->hide();

    scrollToBottomNow();

    // 如果是初始化阶段用预取消息预构建的部分缓存，还需要继续加载完整历史
    if (partial) {
        m_history.reset();
        loadHistory();
    }

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

void DSHHub::startIncrementalHistoryBuild(const QJsonArray &events)
{
    cancelIncrementalHistoryBuild();

    m_pendingBuildHolder = new QWidget;
    m_pendingBuildLayout = new QVBoxLayout(m_pendingBuildHolder);
    m_pendingBuildQuery = new MessageQuery;
    m_pendingBuildEvents = events;
    m_pendingBuildIndex = 0;
    m_pendingBuildActive = true;

    continueIncrementalHistoryBuild();
}

void DSHHub::continueIncrementalHistoryBuild()
{
    if (!m_pendingBuildActive || !m_pendingBuildQuery || !m_pendingBuildLayout)
        return;

    const int batchSize = 5;
    const int end = qMin(m_pendingBuildIndex + batchSize, m_pendingBuildEvents.size());

    QJsonArray batch;
    for (int i = m_pendingBuildIndex; i < end; ++i)
        batch.append(m_pendingBuildEvents.at(i));

    appendHistoryEventsToQuery(m_pendingBuildQuery, m_pendingBuildLayout, batch);
    m_pendingBuildIndex = end;

    if (m_pendingBuildIndex < m_pendingBuildEvents.size()) {
        QTimer::singleShot(0, this, &DSHHub::continueIncrementalHistoryBuild);
        return;
    }

    MessageQuery *query = m_pendingBuildQuery;
    QWidget *holder = m_pendingBuildHolder;
    QVBoxLayout *layout = m_pendingBuildLayout;

    m_pendingBuildQuery = nullptr;
    m_pendingBuildHolder = nullptr;
    m_pendingBuildLayout = nullptr;
    m_pendingBuildActive = false;
    m_pendingBuildEvents = QJsonArray();
    m_pendingBuildIndex = 0;

    query->detachFromLayout(layout);
    query->releaseWidgets();
    delete holder;

    m_usingPrefetched = false;
    hideLoadingIndicator();
    swapToMessageQuery(query);
    finishInitialization();

    if (m_loadMoreButton)
        m_loadMoreButton->setVisible(true);
}

void DSHHub::cancelIncrementalHistoryBuild()
{
    if (!m_pendingBuildActive)
        return;

    if (m_pendingBuildQuery && m_pendingBuildLayout) {
        m_pendingBuildQuery->detachFromLayout(m_pendingBuildLayout);
        m_pendingBuildQuery->releaseWidgets();
    }

    delete m_pendingBuildHolder;
    m_pendingBuildHolder = nullptr;
    m_pendingBuildLayout = nullptr;

    delete m_pendingBuildQuery;
    m_pendingBuildQuery = nullptr;

    m_pendingBuildEvents = QJsonArray();
    m_pendingBuildIndex = 0;
    m_pendingBuildActive = false;
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

    if (!m_prebuildQueue.contains(sessionId))
        m_prebuildQueue.append(sessionId);
    processPrebuildQueue();
}

void DSHHub::processPrebuildQueue()
{
    if (m_prebuilding)
        return;

    m_prebuilding = true;

    while (!m_prebuildQueue.isEmpty()) {
        const QString sessionId = m_prebuildQueue.takeFirst();

        if (sessionId == m_sessionId || m_cacheManager.hasCachedMessages(sessionId))
            continue;

        const QJsonArray events = m_cacheManager.takePrefetchedHistory(sessionId);
        if (events.isEmpty())
            continue;

        // 只预构建少量预取消息，控件树很小，避免初始化时卡顿
        MessageQuery *query = buildOffscreenQuery(events);
        m_cacheManager.cacheSessionMessages(sessionId, query);
        m_cacheManager.markPartialCache(sessionId);
        break;
    }

    m_prebuilding = false;

    if (!m_prebuildQueue.isEmpty())
        QTimer::singleShot(0, this, &DSHHub::processPrebuildQueue);
}


void DSHHub::onSessionSelected(const QString &sessionId)
{
    if (sessionId.isEmpty())
        return;

    cancelIncrementalHistoryBuild();
    hideLoadingIndicator();

    // 先把当前会话的消息控件缓存起来
    cacheCurrentMessages();

    // 再切换到目标会话
    m_sessionId = sessionId;

    if (m_sidebar)
        m_sidebar->workspaceList()->setCurrentSession(sessionId);

    if (m_topBar && m_sidebar)
        m_topBar->setTitle(m_sidebar->workspaceList()->titleForSession(sessionId));

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
    m_sidebar->clearAllSessions(
        m_dshHome,
        [this]() {
            hideLoadingIndicator();
            if (m_messages)
                m_messages->clear();
            m_cacheManager.clearAll();
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
    qInfo().noquote() << QStringLiteral("[DSH Hub] connected to DSH");
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
                if (m_sidebar && m_api)
                    m_sidebar->workspaceList()->refreshTitles(m_api);
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
        } else if (eventType == QStringLiteral("tool/call")) {
            const ToolCallInfo tool = extractToolCall(event);
            if (tool.valid && m_messages) {
                AgentMessageUnit *target = m_messages->lastAgentUnitIfLast();
                if (!target)
                    target = m_messages->addAgentMessage(QString(), m_messagesLayout);
                const QString html = QStringLiteral("<pre>%1</pre>")
                    .arg(QString::fromUtf8(
                             QJsonDocument(tool.arguments).toJson(QJsonDocument::Indented))
                              .toHtmlEscaped());
                target->appendStreamChunk(StreamSegment::ToolCall, html, tool.name);
            }
        } else if (eventType == QStringLiteral("tool/result")) {
            const ToolResultInfo result = extractToolResult(event);
            if (result.valid && m_messages) {
                AgentMessageUnit *target = m_messages->lastAgentUnitIfLast();
                if (!target)
                    target = m_messages->addAgentMessage(QString(), m_messagesLayout);
                const QString html = QStringLiteral("<pre>%1</pre>").arg(result.message.toHtmlEscaped());
                target->appendStreamChunk(StreamSegment::ToolResult, html);
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

    qInfo().noquote() << QStringLiteral("[DSH Hub] loadHistory session=") << m_sessionId;
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
                // 首次加载：增量构建，避免一次性创建大量控件卡顿
                m_history.setEventCount(newCount);
                m_history.setHasMore(newCount >= m_history.limit());
                if (m_loadMoreButton)
                    m_loadMoreButton->hide();
                startIncrementalHistoryBuild(events);
            } else if (newCount > oldCount) {
                m_history.setEventCount(newCount);
                m_history.setHasMore(newCount >= m_history.limit());

                if (m_usingPrefetched) {
                    // 之前先显示了预取的最近几条，现在后台增量构建完整历史，构建完再切换
                    startIncrementalHistoryBuild(events);
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

            if (m_loadMoreButton && !m_pendingBuildActive)
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
            if (m_sidebar)
                m_sidebar->workspaceList()->setWorkspaces(value.value(QStringLiteral("items")).toArray());
            loadSessionList();
        },
        [this](const DshApiClient::RpcError &) {
            if (m_sidebar)
                m_sidebar->workspaceList()->setWorkspaces(QJsonArray());
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
            qInfo().noquote() << QStringLiteral("[DSH Hub] session.list loaded, count=") << items.size();
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

                if (m_sidebar)
                    m_sidebar->workspaceList()->addSession(sid, label);
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
                    if (m_sidebar)
                        m_sidebar->workspaceList()->setCurrentSession(m_sessionId);

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
            if (m_sidebar) {
                m_sidebar->workspaceList()->addSession(m_sessionId, QStringLiteral("未命名会话"));
                m_sidebar->workspaceList()->setCurrentSession(m_sessionId);
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
