#include "DSHHub.h"
#include "ServerManager.h"
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
#include "DshNamedPipeBridge.h"
#include "DllJsonCaller.h"
#include "ExtensionManagerPopup.h"
#include "InteractionHandler.h"

#include "DshEventParser.h"
#include "AgentMessageUnit.h"
#include "MessageQuery.h"
#include "LoadingCard.h"
#include "LoadMoreButton.h"

#include <QCoreApplication>

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QFileDialog>
#include <QFrame>

#include <QHBoxLayout>

#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
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
#include <QSettings>

#include <QTimer>
#include <QUrl>
#include <QTcpSocket>
#include <QLocalSocket>
#include <QVBoxLayout>

DSHHub::DSHHub(QWidget* parent, const QUrl& initialBaseUrl, QProcess* initialServerProcess)
	: QMainWindow(parent)
	, m_api(new DshApiClient(this))
{
	qInfo().noquote() << QStringLiteral("[DSH Hub] constructor started");
	setWindowTitle(QStringLiteral("DSH Hub"));
	setAttribute(Qt::WA_DeleteOnClose);

	QSettings settings;
	m_defaultAgentPreset = settings.value(QStringLiteral("agent/defaultPreset")).toString();

	// 启动命名管道桥接服务，供 Node/DSh server 调用 DLL 工具
	m_pipeBridge = new DshNamedPipeBridge(this);
	connect(m_pipeBridge, &DshNamedPipeBridge::requestReceived,
		this, &DSHHub::handlePipeRequest);
	if (!m_pipeBridge->start()) {
		qWarning() << QStringLiteral("[DSH Pipe] failed to start:") << m_pipeBridge->errorString();
	}

	// 初始化 JSON5 DLL 调用器
	m_dllCaller = new DllJsonCaller;
	const QString appDir = QCoreApplication::applicationDirPath();
	const QString serverProfilePath = appDir + QStringLiteral("/resources/server/harness/profiles/web");
	const QString extensionsRoot = serverProfilePath + QStringLiteral("/extensions");

	QString descriptorPath = qEnvironmentVariable("DSH_DLL_JSON5", QString());
	QString dllPath = qEnvironmentVariable("DSH_DLL", QString());

	// 没有显式指定时，从已安装扩展包里按 regulation.json5 + main.dll 查找并加载
	if (descriptorPath.isEmpty() || dllPath.isEmpty()) {
		const QStringList scanRoots = {
			extensionsRoot,
			serverProfilePath + QStringLiteral("/node_modules")
		};
		for (const QString& scanRoot : scanRoots) {
			QDirIterator it(scanRoot, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
			while (it.hasNext()) {
				const QString dir = it.next();
				const QString candidateJson = dir + QStringLiteral("/regulation.json5");
				const QString candidateDll = dir + QStringLiteral("/main.dll");
				if (QFile::exists(candidateJson) && QFile::exists(candidateDll)) {
					if (descriptorPath.isEmpty())
						descriptorPath = candidateJson;
					if (dllPath.isEmpty())
						dllPath = candidateDll;
					break;
				}
			}
			if (!descriptorPath.isEmpty() && !dllPath.isEmpty())
				break;
		}
	}

	if (!descriptorPath.isEmpty() && !dllPath.isEmpty()
		&& QFile::exists(descriptorPath) && QFile::exists(dllPath)) {
		if (!m_dllCaller->loadDescriptor(descriptorPath)) {
			qWarning() << "[DllCaller] descriptor error:" << m_dllCaller->errorString();
		}
		else if (!m_dllCaller->loadLibrary(dllPath)) {
			qWarning() << "[DllCaller] library error:" << m_dllCaller->errorString();
		}
	}

	// 创建界面
	auto* central = new QWidget(this);
	central->setStyleSheet(QStringLiteral("QWidget {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("windowBg")) + QStringLiteral(";") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QLineEdit {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border: 1px solid ") + QStringLiteral("#D0D7DE") + QStringLiteral(";") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("  padding: 6px 10px;") + QStringLiteral("}") + QStringLiteral("QLineEdit:focus {") + QStringLiteral("  border-color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QPushButton {") + QStringLiteral("  background: ") + QStringLiteral("#E8EAED") + QStringLiteral(";") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("  padding: 6px 14px;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QPushButton:hover {") + QStringLiteral("  background: ") + QStringLiteral("#DDE0E4") + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QPushButton:pressed {") + QStringLiteral("  background: ") + QStringLiteral("#CDD0D5") + QStringLiteral(";") + QStringLiteral("}"));
	auto* layout = new QVBoxLayout(central);

	m_scrollArea = new QScrollArea(central);
	m_scrollArea->setFixedWidth(900);
	m_scrollArea->setFrameShape(QFrame::NoFrame);
	m_scrollArea->setStyleSheet(QStringLiteral("QScrollArea {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("windowBg")) + QStringLiteral(";") + QStringLiteral("  border: none;") + QStringLiteral("}") + QStringLiteral("QScrollArea QScrollBar:vertical {") + QStringLiteral("  background: transparent;") + QStringLiteral("  width: 8px;") + QStringLiteral("  margin: 2px;") + QStringLiteral("}") + QStringLiteral("QScrollArea QScrollBar::handle:vertical {") + QStringLiteral("  background: ") + QStringLiteral("#C1C7CF") + QStringLiteral(";") + QStringLiteral("  border-radius: 4px;") + QStringLiteral("  min-height: 30px;") + QStringLiteral("}") + QStringLiteral("QScrollArea QScrollBar::handle:vertical:hover {") + QStringLiteral("  background: ") + QStringLiteral("#A8B0B9") + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QScrollArea QScrollBar::add-line:vertical,") + QStringLiteral("QScrollArea QScrollBar::sub-line:vertical {") + QStringLiteral("  height: 0;") + QStringLiteral("}") + QStringLiteral("QScrollArea QScrollBar::add-page:vertical,") + QStringLiteral("QScrollArea QScrollBar::sub-page:vertical {") + QStringLiteral("  background: transparent;") + QStringLiteral("}"));

	auto* scrollContent = new QWidget;
	scrollContent->setStyleSheet(QStringLiteral("background: ") + Theme::color(QStringLiteral("windowBg")) + QStringLiteral(";"));
	auto* scrollLayout = new QVBoxLayout(scrollContent);
	scrollLayout->setContentsMargins(10, 0, 0, 0);
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
	auto* rightPanel = new QWidget(central);
	rightPanel->setFixedWidth(900);
	rightPanel->setAttribute(Qt::WA_StyledBackground, true);
	rightPanel->setStyleSheet(QStringLiteral("background: ") + Theme::color(QStringLiteral("windowBg")) + QStringLiteral(";") + QStringLiteral("border-radius: 12px;"));

	m_chatInput = new ChatInputWidget(rightPanel);

	// 底部“没有更多了”提示
	m_toastLabel = new QLabel(QStringLiteral("啊哦，没有更多了"), this);
	m_toastLabel->setStyleSheet(QStringLiteral("QLabel {") + QStringLiteral("  background: rgba(31,35,40,0.85);") + QStringLiteral("  color: white;") + QStringLiteral("  border-radius: 16px;") + QStringLiteral("  padding: 8px 16px;") + QStringLiteral("  font-size: 13px;") + QStringLiteral("}"));
	m_toastLabel->setAlignment(Qt::AlignCenter);
	m_toastLabel->hide();

	auto* panelLayout = new QVBoxLayout(rightPanel);
	panelLayout->setContentsMargins(0, 0, 0, 0);
	panelLayout->setSpacing(0);

	// 左侧灰色会话列表
	m_sidebar = new Sidebar(central);
	m_sidebar->setFixedWidth(240);

	panelLayout->addWidget(m_scrollArea);

	auto* inputLayout = new QHBoxLayout;
	inputLayout->addWidget(m_chatInput, 1);

	inputLayout->setContentsMargins(10, 8, 10, 8);
	inputLayout->setSpacing(8);
	panelLayout->addLayout(inputLayout);

	// 对话顶部栏：宽度与对话栏一致，放在右侧对话栏上方
	auto* rightColumn = new QWidget(central);
	rightColumn->setFixedWidth(900);
	auto* rightColumnLayout = new QVBoxLayout(rightColumn);
	rightColumnLayout->setContentsMargins(0, 0, 0, 0);
	rightColumnLayout->setSpacing(8);

	m_topBar = new TopBar(rightColumn);
	auto* topBarRow = new QWidget(rightColumn);
	auto* topBarRowLayout = new QHBoxLayout(topBarRow);
	topBarRowLayout->setContentsMargins(10, 0, 0, 0);
	topBarRowLayout->setSpacing(0);
	topBarRowLayout->addWidget(m_topBar);
	topBarRowLayout->addStretch();

	rightColumnLayout->addWidget(topBarRow);
	rightColumnLayout->addWidget(rightPanel, 1);

	auto* bodyLayout = new QHBoxLayout;
	bodyLayout->setSpacing(0);

	bodyLayout->addWidget(m_sidebar);
	bodyLayout->addWidget(rightColumn);

	// 内容容器：整体居中
	auto* content = new QWidget(central);
	content->setFixedWidth(1140);
	auto* contentLayout = new QVBoxLayout(content);
	contentLayout->setContentsMargins(0, 0, 0, 0);
	contentLayout->addLayout(bodyLayout, 1);

	layout->addWidget(content, 0, Qt::AlignHCenter);
	// 输入框是 rightPanel 的子控件，随右侧面板一起布局，无需加入主布局

	setCentralWidget(central);

	setMinimumWidth(1160);
	resize(1000, 700);

	// 初始化灰色蒙版 + 居中标签
	m_initOverlay = new QWidget(this);
	m_initOverlay->setObjectName(QStringLiteral("initOverlay"));
	m_initOverlay->setStyleSheet(QStringLiteral("QWidget#initOverlay {") + QStringLiteral("  background-color: rgba(128,128,128,0.65);") + QStringLiteral("}"));
	auto* overlayLayout = new QVBoxLayout(m_initOverlay);

	// 现代化横版卡片：宽高比约 5:3
	auto* initCard = new QWidget(m_initOverlay);
	initCard->setFixedSize(400, 240);
	initCard->setStyleSheet(QStringLiteral("QWidget {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border-radius: 20px;") + QStringLiteral("}"));

	auto* cardLayout = new QVBoxLayout(initCard);
	cardLayout->setContentsMargins(24, 20, 24, 20);
	cardLayout->setSpacing(8);

	// 卡片内顶部显示 Logo
	auto* cardLogo = new QLabel(initCard);
	cardLogo->setAlignment(Qt::AlignCenter);
	cardLogo->setAttribute(Qt::WA_TranslucentBackground);
	const QString cardLogoResource = Theme::isDark()
		? QStringLiteral(":/DSHHub/DSH-Hub-Logo-Tiny-Dark@2x.png")
		: QStringLiteral(":/DSHHub/DSH-Hub-Logo-Tiny@2x.png");
	QPixmap cardLogoPix(cardLogoResource);
	if (!cardLogoPix.isNull()) {
		cardLogoPix.setDevicePixelRatio(2.0);
		cardLogo->setPixmap(cardLogoPix);
	}
	else {
		cardLogo->setText(QStringLiteral("DSH Hub"));
	}
	cardLayout->addWidget(cardLogo);

	// 下方：旋转条 + 初始化文字
	auto* rowLayout = new QHBoxLayout;
	rowLayout->setSpacing(16);

	auto* spinner = new SpinnerWidget(initCard);
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
		AgentMessageUnit* target = m_messages ? m_messages->lastAgentUnitIfLast() : nullptr;
		if (!target)
			return;
		target->flushStream();
		if (m_scrollArea && m_scrollArea->verticalScrollBar()) {
			QScrollBar* bar = m_scrollArea->verticalScrollBar();
			if (bar->value() >= bar->maximum() - 80)
				scrollToBottomNow();
		}
		});

	// 信号槽
	connect(m_chatInput, &ChatInputWidget::sendRequested, this, &DSHHub::onSendClicked);
	connect(m_chatInput, &ChatInputWidget::stopRequested, this, &DSHHub::onStopRequested);

	connect(m_sidebar, &Sidebar::newWorkspaceRequested,
		this, &DSHHub::onNewWorkspaceClicked);
	connect(m_sidebar, &Sidebar::createSessionInWorkspaceRequested,
		this, &DSHHub::onCreateSessionInWorkspace);
	connect(m_sidebar, &Sidebar::sessionSelected,
		this, &DSHHub::onSessionSelected);
	connect(m_sidebar, &Sidebar::deleteSessionRequested,
		this, &DSHHub::onDeleteSessionRequested);
	connect(m_sidebar, &Sidebar::clearRequested,
		this, &DSHHub::onClearConversationClicked);

	connect(m_api, &DshApiClient::connected, this, &DSHHub::handleConnected);
	connect(m_api, &DshApiClient::muxFrameReceived, this, &DSHHub::handleMuxFrame);
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
	connect(m_sidebar, &Sidebar::extensionsRequested,
		this, &DSHHub::openExtensions);
	connect(m_sidebar, &Sidebar::initialSessionReady,
		this, &DSHHub::onInitialSessionReady);
	connect(m_sidebar, &Sidebar::sessionCreated,
		this, &DSHHub::onSessionCreated);
	connect(m_sidebar, &Sidebar::noSessionAvailable,
		this, &DSHHub::onNoSessionAvailable);
	connect(m_sidebar, &Sidebar::sessionListError,
		this, &DSHHub::onSessionListError);
	connect(m_sidebar, &Sidebar::sessionCreateError,
		this, &DSHHub::onSessionCreateError);

	m_historyLoader = new HistoryLoader(m_api, m_messages, m_messagesLayout, &m_history, m_scrollArea, this);
	connect(m_historyLoader, &HistoryLoader::loadingChanged,
		this, &DSHHub::onHistoryLoadingChanged);
	connect(m_historyLoader, &HistoryLoader::loadMoreButtonVisibleChanged,
		this, &DSHHub::onHistoryLoadMoreButtonVisibleChanged);
	connect(m_historyLoader, &HistoryLoader::historyError,
		this, &DSHHub::onHistoryError);
	connect(m_historyLoader, &HistoryLoader::incrementalBuildReady,
		this, &DSHHub::onIncrementalBuildReady);
	connect(m_historyLoader, &HistoryLoader::noMoreHistory, this, [this]() {
		showNoMoreToast();
		if (m_loadMoreButton)
			m_loadMoreButton->hide();
		});
	connect(m_loadMoreButton, &QPushButton::clicked, m_historyLoader, &HistoryLoader::loadMore);

	// 服务端统一交给 ServerManager 管理
	m_serverManager = new ServerManager(this);
	connect(m_serverManager, &ServerManager::baseUrlReady, this, [this](const QUrl& url) {
		if (!m_api)
			return;
		m_api->setBaseUrl(url);
		if (m_pluginsPopup)
			m_pluginsPopup->setBaseUrl(url);
		m_api->openStreams();
		});
	connect(m_serverManager, &ServerManager::errorLine, this, [this](const QString& line) {
		if (m_messages) {
			m_messages->addSystemMessage(
				QStringLiteral("DSH 服务端: %1").arg(line),
				m_messagesLayout);
		}
		// 移除扩展后服务端启动失败时，自动清理 cordis.patch.yml 残留
		if (m_cleanupResidualsAfterServerError && m_extensionPopup) {
			m_cleanupResidualsAfterServerError = false;
			m_extensionPopup->cleanupResiduals();
		}
		if (!isInitializationComplete())
			finishInitialization();
		});
	connect(m_serverManager, &ServerManager::outputLine, this, [](const QString& line) {
		// 只记录插件市场/registry/snapshot 相关输出和错误，避免刷爆日志
		if (line.contains(QStringLiteral("dshmarket"), Qt::CaseInsensitive)
			|| line.contains(QStringLiteral("registry"), Qt::CaseInsensitive)
			|| line.contains(QStringLiteral("snapshot"), Qt::CaseInsensitive)
			|| line.contains(QStringLiteral("error"), Qt::CaseInsensitive)
			|| line.contains(QStringLiteral("fail"), Qt::CaseInsensitive)) {
			qInfo().noquote() << "[DSH Server]" << line;
		}
		});
	connect(m_serverManager, &ServerManager::finished, this, [this](int exitCode, QProcess::ExitStatus) {
		if (m_serverManager && m_serverManager->isRestarting())
			return;

		if (m_api && !m_api->isConnected()) {
			if (m_messages) {
				m_messages->addSystemMessage(
					QStringLiteral("DSH 服务端已退出，代码: %1").arg(exitCode),
					m_messagesLayout);
			}
			if (m_cleanupResidualsAfterServerError && m_extensionPopup) {
				m_cleanupResidualsAfterServerError = false;
				m_extensionPopup->cleanupResiduals();
			}
			if (exitCode != 0 && !isInitializationComplete())
				finishInitialization();
		}
		});

	m_serverManager->start(initialBaseUrl, initialServerProcess);
}

void DSHHub::resizeEvent(QResizeEvent* event)
{
	QMainWindow::resizeEvent(event);

	if (m_initOverlay)
		m_initOverlay->setGeometry(rect());

	if (m_settingsOverlay)
		m_settingsOverlay->setGeometry(rect());

	if (m_pluginsOverlay)
		m_pluginsOverlay->setGeometry(rect());
	if (m_extensionOverlay)
		m_extensionOverlay->setGeometry(rect());
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

QUrl DSHHub::baseUrl() const
{
	return m_api ? m_api->baseUrl() : QUrl();
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

	m_settings = new Settings(m_serverManager->dshHome(), m_api, this);
	m_settings->move(geometry().center() - m_settings->rect().center());
	m_settings->show();

	// 仅当 credentials.set 失败时，才通过重启服务端兜底
	connect(m_settings, &Settings::apiKeyChanged, this, [this]() {
		if (m_serverManager)
			m_serverManager->restart();
		});

	connect(m_settings, &Settings::agentPresetChanged, this, [this](const QString& presetId) {
		m_defaultAgentPreset = presetId;
		if (!m_sessionId.isEmpty() && m_api) {
			QJsonObject payload;
			payload.insert(QStringLiteral("sessionId"), m_sessionId);
			payload.insert(QStringLiteral("agentPreset"), presetId);
			m_api->callMethod(QStringLiteral("agentPreset.select"), payload, {}, {});
		}
		});

	connect(m_settings, &Settings::serverSettingsSaved, this, [this]() {
		if (m_serverManager)
			m_serverManager->restart();
		});

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

	connect(m_pluginsPopup, &PluginsPopup::serverRestartRequested,
		m_serverManager, &ServerManager::restart);

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

QProcess* DSHHub::takeServerProcess()
{
	return m_serverManager ? m_serverManager->takeProcess() : nullptr;
}

void DSHHub::adoptServerProcess(QProcess* process)
{
	if (m_serverManager)
		m_serverManager->adoptProcess(process);
}

DSHHub::~DSHHub()
{
	if (m_api)
		disconnect(m_api, nullptr, this, nullptr);

	delete m_dllCaller;
	delete m_messages;
	m_messages = nullptr;

	m_cacheManager.clearAll();
}

void DSHHub::onSendClicked()
{
	const QString text = m_chatInput->text().trimmed();
	if (text.isEmpty())
		return;

	if (m_sessionId.isEmpty()) {
		m_messages->addSystemMessage(QStringLiteral("还没有可用会话，正在自动创建..."),
			m_messagesLayout);
		createSessionAndSend(text);
		return;
	}

	sendPrompt(text);
}

void DSHHub::onStopRequested()
{
	if (!m_streaming)
		return;

	if (!m_api || m_sessionId.isEmpty()) {
		// 没有可用会话时也把本地流式状态复位，避免按钮卡在中止态
		m_streaming = false;
		updateStreamingUi();
		return;
	}

	QJsonObject payload;
	payload.insert(QStringLiteral("sessionId"), m_sessionId);

	m_api->callMethod(
		QStringLiteral("session.cancel"),
		payload,
		[this](const QJsonObject&) {
			m_streaming = false;
			if (m_streamTimer)
				m_streamTimer->stop();
			if (m_messages && m_messages->lastAgentUnitIfLast())
				m_messages->lastAgentUnitIfLast()->flushStream();
			updateStreamingUi();
		},
		[this](const DshApiClient::RpcError& error) {
			// cancel 失败也恢复按钮状态，避免 UI 一直卡在中止态
			m_streaming = false;
			if (m_streamTimer)
				m_streamTimer->stop();
			if (m_messages && m_messages->lastAgentUnitIfLast())
				m_messages->lastAgentUnitIfLast()->flushStream();
			updateStreamingUi();
			if (m_messages) {
				m_messages->addSystemMessage(
					QStringLiteral("中止失败: %1 %2").arg(error.code, error.message),
					m_messagesLayout);
			}
		});
}

void DSHHub::updateStreamingUi()
{
	if (m_chatInput)
		m_chatInput->setStreaming(m_streaming);
}

void DSHHub::clearInteractionPanels()
{
	for (QWidget* panel : m_interactionPanels) {
		if (!panel)
			continue;

		if (m_messagesLayout)
			m_messagesLayout->removeWidget(panel);
		panel->hide();
		panel->deleteLater();
	}
	m_interactionPanels.clear();
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
		[this](const QJsonObject&) {
			if (m_sidebar && m_api && m_prefetcher)
				m_sidebar->refreshSessions(m_api, m_prefetcher);
		},
		[this](const DshApiClient::RpcError& error) {
			if (m_messages) {
				m_messages->addSystemMessage(
					QStringLiteral("新建工作区失败: %1 %2").arg(error.code, error.message),
					m_messagesLayout);
			}
		});
}

void DSHHub::onCreateSessionInWorkspace(const QString& workspaceId)
{
	QJsonObject payload;
	if (!workspaceId.isEmpty())
		payload.insert(QStringLiteral("workspaceId"), workspaceId);
	if (!m_defaultAgentPreset.isEmpty())
		payload.insert(QStringLiteral("agentPreset"), m_defaultAgentPreset);

	m_api->callMethod(
		QStringLiteral("session.create"),
		payload,
		[this, workspaceId](const QJsonObject& value) {
			const QString newSessionId = value.value(QStringLiteral("sessionId")).toString();
			if (newSessionId.isEmpty())
				return;

			cacheCurrentMessages();
			m_messages = new MessageQuery;
			m_sessionId = newSessionId;
			if (m_topBar)
				m_topBar->setTitle(QStringLiteral("未命名会话"));
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSessionToWorkspace(newSessionId, QStringLiteral("未命名会话"), workspaceId);
				m_sidebar->workspaceList()->setCurrentSession(newSessionId);
			}

			m_history.reset();
			m_usingPrefetched = false;
			if (m_loadMoreButton)
				m_loadMoreButton->hide();

			if (m_historyLoader) {
				m_historyLoader->setMessages(m_messages);
				m_historyLoader->setUsingPrefetched(false);
				m_historyLoader->load(newSessionId);
			}
		},
		[this](const DshApiClient::RpcError& error) {
			if (m_messages) {
				m_messages->addSystemMessage(
					QStringLiteral("新建会话失败: %1 %2").arg(error.code, error.message),
					m_messagesLayout);
			}
		});
}

void DSHHub::sendPrompt(const QString& text)
{
	if (m_sessionId.isEmpty() || !m_messages)
		return;

	m_streaming = false;
	if (m_streamTimer)
		m_streamTimer->stop();
	updateStreamingUi();
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
		[this](const QJsonObject&) {
		},
		[this](const DshApiClient::RpcError& error) {
			if (m_messages) {
				m_messages->addSystemMessage(
					QStringLiteral("发送失败: %1 %2").arg(error.code, error.message),
					m_messagesLayout);
			}
		});

	m_chatInput->clear();
}

void DSHHub::createSessionAndSend(const QString& text)
{
	if (!m_api)
		return;

	QJsonObject payload;
	if (!m_defaultAgentPreset.isEmpty())
		payload.insert(QStringLiteral("agentPreset"), m_defaultAgentPreset);
	m_api->callMethod(
		QStringLiteral("session.create"),
		payload,
		[this, text](const QJsonObject& value) {
			const QString sid = value.value(QStringLiteral("sessionId")).toString();
			if (sid.isEmpty())
				return;

			if (m_historyLoader)
				m_historyLoader->cancelBuild();
			cacheCurrentMessages();
			m_messages = new MessageQuery;
			m_sessionId = sid;
			m_history.reset();

			if (m_topBar)
				m_topBar->setTitle(QStringLiteral("未命名会话"));
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSession(sid, QStringLiteral("未命名会话"));
				m_sidebar->workspaceList()->setCurrentSession(sid);
			}
			if (m_loadMoreButton)
				m_loadMoreButton->hide();

			m_usingPrefetched = false;
			if (m_historyLoader) {
				m_historyLoader->setMessages(m_messages);
				m_historyLoader->setUsingPrefetched(false);
			}

			sendPrompt(text);
		},
		[this](const DshApiClient::RpcError& error) {
			if (m_messages) {
				m_messages->addSystemMessage(
					QStringLiteral("创建会话失败: %1 %2").arg(error.code, error.message),
					m_messagesLayout);
			}
		});
}

void DSHHub::cacheCurrentMessages()
{
	m_cacheManager.cacheOrDiscardCurrentSession(m_sessionId, m_messages, m_messagesLayout);
	m_messages = nullptr;
}

bool DSHHub::tryRestoreCachedMessages(const QString& sessionId)
{
	const bool partial = m_cacheManager.isPartialCache(sessionId);
	m_messages = m_cacheManager.restoreCachedSession(sessionId, m_messagesLayout);
	if (!m_messages)
		return false;

	hideLoadingIndicator();
	if (m_loadMoreButton)
		m_loadMoreButton->setVisible(!m_messages->messages.empty());

	scrollToBottomNow();

	// If this is a partial cache built from prefetched history, continue loading full history.
	if (partial) {
		m_history.reset();
		if (m_historyLoader) {
			m_historyLoader->setMessages(m_messages);
			m_historyLoader->load(sessionId);
		}
	}

	return true;
}

void DSHHub::swapToMessageQuery(MessageQuery* query)
{
	clearInteractionPanels();
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
		QWidget* content = m_scrollArea->widget();
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

void DSHHub::onHistoryPrefetched(const QString& sessionId, const QJsonArray& events)
{
	if (sessionId.isEmpty() || events.isEmpty())
		return;

	// If the prefetched history belongs to the currently opened session, show it immediately.
	if (sessionId == m_sessionId) {
		if (!m_messages || !m_messages->messages.empty())
			return;
		m_messages->appendEvents(m_messagesLayout, events);
		m_history.setEventCount(events.size());
		m_usingPrefetched = true;
		hideLoadingIndicator();
		scrollToBottomNow();
		if (m_historyLoader)
			m_historyLoader->setUsingPrefetched(true);
		return;
	}

	if (m_cacheManager.hasCachedMessages(sessionId))
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
		MessageQuery* query = MessageQuery::fromEvents(events);
		m_cacheManager.cacheSessionMessages(sessionId, query);
		m_cacheManager.markPartialCache(sessionId);
		break;
	}

	m_prebuilding = false;

	if (!m_prebuildQueue.isEmpty())
		QTimer::singleShot(0, this, &DSHHub::processPrebuildQueue);
}

void DSHHub::onSessionSelected(const QString& sessionId)
{
	if (sessionId.isEmpty())
		return;

	hideLoadingIndicator();
	// 切换会话时先取消上一个会话尚未完成的增量构建，避免旧消息覆盖新会话
	if (m_historyLoader)
		m_historyLoader->cancelBuild();
	// 切换会话时停止旧会话的流式渲染状态，避免旧输出继续污染新会话
	m_streaming = false;
	if (m_streamTimer)
		m_streamTimer->stop();
	updateStreamingUi();
	clearInteractionPanels();
	cacheCurrentMessages();

	m_sessionId = sessionId;

	if (m_sidebar)
		m_sidebar->workspaceList()->setCurrentSession(sessionId);

	if (m_topBar && m_sidebar)
		m_topBar->setTitle(m_sidebar->workspaceList()->titleForSession(sessionId));

	if (tryRestoreCachedMessages(sessionId))
		return;

	m_messages = new MessageQuery;
	m_history.setLimit(20);
	m_history.setHasMore(false);

	const QJsonArray prefetched = m_cacheManager.takePrefetchedHistory(sessionId);
	m_usingPrefetched = !prefetched.isEmpty();
	if (!prefetched.isEmpty()) {
		m_history.setEventCount(prefetched.size());
		m_messages->appendEvents(m_messagesLayout, prefetched);
		scrollToBottomNow();
	}
	else {
		m_history.setEventCount(0);
	}

	if (m_loadMoreButton)
		m_loadMoreButton->hide();

	if (m_historyLoader) {
		m_historyLoader->setMessages(m_messages);
		m_historyLoader->setUsingPrefetched(m_usingPrefetched);
		m_historyLoader->load(sessionId);
	}
}

void DSHHub::onDeleteSessionRequested(const QString& sessionId)
{
	if (sessionId.isEmpty() || !m_api)
		return;

	const auto ret = QMessageBox::question(
		this,
		QStringLiteral("删除会话"),
		QStringLiteral("确定要删除这个会话吗？此操作无法撤销。"),
		QMessageBox::Yes | QMessageBox::No,
		QMessageBox::No);
	if (ret != QMessageBox::Yes)
		return;

	QJsonObject payload;
	payload.insert(QStringLiteral("sessionId"), sessionId);

	m_api->callMethod(
		QStringLiteral("workspace.archiveSession"),
		payload,
		[this, sessionId](const QJsonObject&) {
			qInfo().noquote() << "[DSH Hub] session archived:" << sessionId;

			// archiveSession 只更新归档状态，这里同时删除本地会话文件，避免重启后残留
			if (m_serverManager) {
				const QString sessionsRoot = m_serverManager->dshHome() + QStringLiteral("/sessions");
				QDir sessionsDir(sessionsRoot);
				if (sessionsDir.exists()) {
					const QStringList workspaceDirs = sessionsDir.entryList(
						QDir::Dirs | QDir::NoDotAndDotDot);
					for (const QString& workspaceDir : workspaceDirs) {
						QDir candidate(sessionsDir.filePath(workspaceDir));
						if (candidate.exists(sessionId)) {
							QDir sessionDir(candidate.filePath(sessionId));
							if (sessionDir.removeRecursively()) {
								qInfo().noquote() << "[DSH Hub] session files removed:"
									<< sessionDir.absolutePath();
							}
							else {
								qWarning().noquote() << "[DSH Hub] failed to remove session files:"
									<< sessionDir.absolutePath();
							}
						}
					}
				}
			}

			if (sessionId == m_sessionId) {
				// 删除当前会话时也停止流式渲染
				m_streaming = false;
				if (m_streamTimer)
					m_streamTimer->stop();
				updateStreamingUi();
				clearInteractionPanels();
				// 当前会话被删除时，先丢弃当前消息容器，避免继续持有已归档会话
				m_cacheManager.cacheOrDiscardCurrentSession(QString(), m_messages, m_messagesLayout);
				m_messages = new MessageQuery;
				m_sessionId.clear();
				m_history.reset();
				if (m_loadMoreButton)
					m_loadMoreButton->hide();
				if (m_topBar)
					m_topBar->setTitle(QStringLiteral("New session"));
			}

			if (m_sidebar && m_api && m_prefetcher)
				m_sidebar->refreshSessions(m_api, m_prefetcher);
		},
		[this](const DshApiClient::RpcError& error) {
			qWarning().noquote() << "[DSH Hub] delete session failed:"
				<< error.code << error.message;
			if (m_messages) {
				m_messages->addSystemMessage(
					QStringLiteral("删除会话失败: %1 %2").arg(error.code, error.message),
					m_messagesLayout);
			}
		});
}

void DSHHub::hideLoadingIndicator()
{
	if (!m_loadingContainer && !m_loadingCard)
		return;

	if (m_messagesLayout && m_loadingContainer) {
		m_messagesLayout->removeWidget(m_loadingContainer);
		m_loadingContainer->deleteLater();
	}
	else if (m_messagesLayout && m_loadingCard) {
		m_messagesLayout->removeWidget(m_loadingCard);
		m_loadingCard->deleteLater();
	}

	m_loadingContainer = nullptr;
	m_loadingCard = nullptr;
}

void DSHHub::onClearConversationClicked()
{
	m_sidebar->clearAllSessions(
		m_serverManager->dshHome(),
		[this]() {
			hideLoadingIndicator();
			clearInteractionPanels();
			if (m_messages)
				m_messages->clear();
			m_cacheManager.clearAll();
			m_sessionId.clear();

			if (m_messages) {
				if (AgentMessageUnit* agent = m_messages->lastAgentUnit())
					agent->clearStreamSegments();
			}
		},
		[this]() {
			callSessionCreate();
		});
}

void DSHHub::callSessionCreate()
{
	if (!m_api)
		return;

	QJsonObject payload;
	if (!m_defaultAgentPreset.isEmpty())
		payload.insert(QStringLiteral("agentPreset"), m_defaultAgentPreset);
	m_api->callMethod(
		QStringLiteral("session.create"),
		payload,
		[this](const QJsonObject& value) {
			const QString sid = value.value(QStringLiteral("sessionId")).toString();
			if (sid.isEmpty())
				return;

			cacheCurrentMessages();
			m_messages = new MessageQuery;
			m_sessionId = sid;
			m_history.reset();

			if (m_topBar)
				m_topBar->setTitle(QStringLiteral("未命名会话"));
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSession(sid, QStringLiteral("未命名会话"));
				m_sidebar->workspaceList()->setCurrentSession(sid);
			}
			if (m_loadMoreButton)
				m_loadMoreButton->hide();

			m_usingPrefetched = false;
			if (m_historyLoader) {
				m_historyLoader->setMessages(m_messages);
				m_historyLoader->setUsingPrefetched(false);
				m_historyLoader->load(sid);
			}
		},
		[this](const DshApiClient::RpcError& error) {
			finishInitialization();
			if (m_messages)
				m_messages->addSystemMessage(
					QStringLiteral("创建会话失败: %1 %2").arg(error.code, error.message),
					m_messagesLayout);
		});
}

void DSHHub::handleConnected()
{
	qInfo().noquote() << QStringLiteral("[DSH Hub] connected to DSH");
	if (m_sidebar && m_api && m_prefetcher)
		m_sidebar->refreshSessions(m_api, m_prefetcher);
}

void DSHHub::onInitialSessionReady(const QString& sessionId, const QString& title)
{
	Q_UNUSED(title)
		onSessionSelected(sessionId);
}

void DSHHub::onSessionCreated(const QString& sessionId, const QString& workspaceId)
{
	// Cache the current session before switching to the new session.
	cacheCurrentMessages();
	m_messages = new MessageQuery;
	m_sessionId = sessionId;
	if (!m_defaultAgentPreset.isEmpty() && m_api) {
		QJsonObject presetPayload;
		presetPayload.insert(QStringLiteral("sessionId"), sessionId);
		presetPayload.insert(QStringLiteral("agentPreset"), m_defaultAgentPreset);
		m_api->callMethod(QStringLiteral("agentPreset.select"), presetPayload, {}, {});
	}
	m_history.reset();

	if (m_sidebar)
		m_sidebar->addCreatedSession(sessionId, workspaceId);
	if (m_sidebar)
		m_sidebar->workspaceList()->setCurrentSession(sessionId);
	if (m_topBar)
		m_topBar->setTitle(QStringLiteral("New session"));
	if (m_loadMoreButton)
		m_loadMoreButton->hide();

	if (m_historyLoader) {
		m_historyLoader->setMessages(m_messages);
		m_historyLoader->load(sessionId);
	}
}

void DSHHub::onNoSessionAvailable()
{
	if (m_sidebar && m_api)
		m_sidebar->createSession(m_api);
}

void DSHHub::onSessionListError(const QString& code, const QString& message)
{
	if (m_messages)
		m_messages->addSystemMessage(QStringLiteral("Session list error: %1 %2").arg(code, message), m_messagesLayout);
	finishInitialization();
}

void DSHHub::onSessionCreateError(const QString& code, const QString& message)
{
	if (m_messages)
		m_messages->addSystemMessage(QStringLiteral("Session create error: %1 %2").arg(code, message), m_messagesLayout);
	finishInitialization();
}

void DSHHub::onHistoryLoadingChanged(bool loading)
{
	Q_UNUSED(loading)
		hideLoadingIndicator();
}

void DSHHub::onHistoryLoadMoreButtonVisibleChanged(bool visible)
{
	if (m_loadMoreButton)
		m_loadMoreButton->setVisible(visible);
}

void DSHHub::onHistoryError(const QString& code, const QString& message)
{
	if (m_messages)
		m_messages->addSystemMessage(QStringLiteral("History error: %1 %2").arg(code, message), m_messagesLayout);
}

void DSHHub::onIncrementalBuildReady(MessageQuery* query)
{
	if (!query)
		return;
	swapToMessageQuery(query);
	finishInitialization();
}

void DSHHub::openExtensions()
{
	if (m_extensionPopup)
		return;

	m_extensionOverlay = new QWidget(this);
	m_extensionOverlay->setObjectName(QStringLiteral("extensionOverlay"));
	m_extensionOverlay->setAttribute(Qt::WA_StyledBackground, true);
	m_extensionOverlay->setStyleSheet(QStringLiteral("QWidget#extensionOverlay { background-color: rgba(128,128,128,0.65); }"));
	m_extensionOverlay->setGeometry(rect());
	m_extensionOverlay->raise();
	m_extensionOverlay->show();

	m_extensionPopup = new ExtensionManagerPopup(m_serverManager->dshHome() + QStringLiteral("/profiles/web"), this);
	m_extensionPopup->move(geometry().center() - m_extensionPopup->rect().center());
	m_extensionPopup->show();

	connect(m_extensionPopup, &ExtensionManagerPopup::serverRestartRequested,
		m_serverManager, &ServerManager::restart);
	connect(m_extensionPopup, &ExtensionManagerPopup::extensionInstalled,
		this, [this](const QString& jsonPath, const QString& dllPath) {
			if (m_dllCaller && m_dllCaller->loadDescriptor(jsonPath) && m_dllCaller->loadLibrary(dllPath)) {
				qInfo().noquote() << "[DSH DllCaller] ready tools=" << m_dllCaller->tools().join(',');
			}
			else if (m_dllCaller) {
				qWarning().noquote() << "[DSH DllCaller] load failed:" << m_dllCaller->errorString();
			}
		});
	connect(m_extensionPopup, &ExtensionManagerPopup::extensionRemoving,
		this, [this](const QString&) {
			// 移除扩展前先卸载 DLL，释放文件占用
			if (m_dllCaller)
				m_dllCaller->unloadLibrary();
			// 如果移除后服务端因残留配置启动失败，自动清理一次
			m_cleanupResidualsAfterServerError = true;
		});
	connect(m_extensionPopup, &PopupWindow::closed, this, [this]() {
		if (m_extensionOverlay) {
			m_extensionOverlay->hide();
			m_extensionOverlay->deleteLater();
			m_extensionOverlay = nullptr;
		}
		if (m_extensionPopup) {
			m_extensionPopup->deleteLater();
			m_extensionPopup = nullptr;
		}
		});
}

void DSHHub::handlePipeRequest(int id, const QString& tool, const QJsonObject& args, QLocalSocket* socket)
{
	QJsonObject result;
	QString error;
	if (m_dllCaller && m_dllCaller->callTool(tool, args, result, &error)) {
		if (m_pipeBridge)
			m_pipeBridge->sendResponse(socket, id, true, result);
	}
	else {
		if (m_pipeBridge)
			m_pipeBridge->sendResponse(socket, id, false, QJsonObject(), error);
	}
}

void DSHHub::handleMuxFrame(const QJsonObject& frame)
{
	const QJsonObject payload = frame.value(QStringLiteral("payload")).toObject();
	const QString type = payload.value(QStringLiteral("type")).toString();
	const QString frameSessionId = payload.value(QStringLiteral("sessionId")).toString();

	if (type == QStringLiteral("session/event")) {
		const QJsonObject event = payload.value(QStringLiteral("event")).toObject();
		// 防止在 A 会话输出时切到 B 会话，A 的流式内容错误地显示到 B 里；
		// 同时把 A 标记为 partial cache，切回 A 时会重新拉取历史，不丢失后台输出
		if (!frameSessionId.isEmpty() && frameSessionId != m_sessionId) {
			m_cacheManager.markPartialCache(frameSessionId);
			return;
		}
		const QString eventType = event.value(QStringLiteral("type")).toString();

		if (eventType == QStringLiteral("assistant/message")) {
			const QString thinking = extractThinking(event);

			const QString reply = extractReply(event);

			if (m_streaming) {
				// 已经通过 assistant/chunk 流式显示过，最终消息不再重复追加
				m_streaming = false;
				if (m_streamTimer)
					m_streamTimer->stop();
				updateStreamingUi();
				if (AgentMessageUnit* target = m_messages->lastAgentUnitIfLast())
					target->flushStream();
			}
			else {
				// 如果没有流式 chunk，也要先把已积压的工具调用/结果渲染出来
				if (AgentMessageUnit* target = m_messages->lastAgentUnitIfLast())
					target->flushStream();

				if (!thinking.isEmpty() || !reply.isEmpty()) {
					// 如果上一条仍然是 Agent 消息，就继续追加到同一个气泡里，保持连续
					AgentMessageUnit* target = m_messages->lastAgentUnitIfLast();
					if (target) {
						if (!thinking.isEmpty()) {
							target->appendThinking(thinking);
						}
						if (!reply.isEmpty())
							target->appendMarkdownWithCodeShadow(reply);
					}
					else {
						// 否则创建新的 AgentMessageUnit
						m_messages->addAgentMessage(reply, m_messagesLayout, thinking);
					}
				}
			}
			// 一次对话完成后，刷新会话标题（如果服务端已经生成了标题）
			if (m_sidebar && m_api)
				m_sidebar->workspaceList()->refreshTitles(m_api);
		}
		else if (eventType == QStringLiteral("assistant/chunk")) {
			const QString chunk = extractEventText(event);

			{
				const QString chunkType = extractChunkType(event);

				AgentMessageUnit* streamTarget = m_messages->lastAgentUnitIfLast();
				if (!streamTarget)
					streamTarget = m_messages->addAgentMessage(QString(), m_messagesLayout);

				if (chunkType == QStringLiteral("reasoning-delta") && !chunk.isEmpty()) {
					streamTarget->appendStreamChunk(StreamSegment::Thinking, chunk);
				}
				else if (chunkType == QStringLiteral("text-delta") && !chunk.isEmpty()) {
					streamTarget->appendStreamChunk(StreamSegment::Reply, chunk);
				}
				// 节流 50ms 批量全量重渲染一次，保证 Markdown/HTML 即时显示
				if (m_streamTimer && !m_streamTimer->isActive())
					m_streamTimer->start();

				m_streaming = true;
				updateStreamingUi();
			}
		}
		else if (eventType == QStringLiteral("text-chunks") || eventType == QStringLiteral("reasoning-chunks")) {
			const QJsonObject eventData = event.value(QStringLiteral("data")).toObject();
			const QJsonArray texts = eventData.value(QStringLiteral("texts")).toArray();
			if (!m_messages || texts.isEmpty())
				return;
			AgentMessageUnit* target = m_messages->lastAgentUnitIfLast();
			if (!target)
				target = m_messages->addAgentMessage(QString(), m_messagesLayout);
			const bool thinking = eventType == QStringLiteral("reasoning-chunks");
			for (const auto& value : texts) {
				const QString chunk = value.toString();
				if (chunk.isEmpty())
					continue;
				target->appendStreamChunk(thinking ? StreamSegment::Thinking : StreamSegment::Reply, chunk);
			}
			if (m_streamTimer && !m_streamTimer->isActive())
				m_streamTimer->start();
			m_streaming = true;
			updateStreamingUi();
		}
		else if (eventType == QStringLiteral("user/message")) {
			// 用户消息已在 onSendClicked 中创建 UserMessageUnit，这里避免重复显示。
		}
		else if (eventType == QStringLiteral("tool/call")) {
			const ToolCallInfo tool = extractToolCall(event);
			if (tool.valid && m_messages) {
				AgentMessageUnit* target = m_messages->lastAgentUnitIfLast();
				if (!target)
					target = m_messages->addAgentMessage(QString(), m_messagesLayout);
				const QString html = QStringLiteral("<pre>%1</pre>")
					.arg(QString::fromUtf8(
						QJsonDocument(tool.arguments).toJson(QJsonDocument::Indented))
						.toHtmlEscaped());
				target->appendStreamChunk(StreamSegment::ToolCall, html, tool.name);
				// 工具调用即使没有 assistant/chunk 也要能显示出来
				if (m_streamTimer && !m_streamTimer->isActive())
					m_streamTimer->start();
			}
		}
		else if (eventType == QStringLiteral("tool/result")) {
			const ToolResultInfo result = extractToolResult(event);
			if (result.valid && m_messages) {
				AgentMessageUnit* target = m_messages->lastAgentUnitIfLast();
				if (!target)
					target = m_messages->addAgentMessage(QString(), m_messagesLayout);
				const QString html = QStringLiteral("<pre>%1</pre>").arg(result.message.toHtmlEscaped());
				target->appendStreamChunk(StreamSegment::ToolResult, html);
				// 工具结果即使没有 assistant/chunk 也要能显示出来
				if (m_streamTimer && !m_streamTimer->isActive())
					m_streamTimer->start();
			}
		}
	}
	else if (type == QStringLiteral("question/requested")) {
		QWidget* panel = InteractionHandler::handleQuestion(frame, m_api, m_messagesLayout);
		if (panel) {
			m_interactionPanels.append(panel);
			scrollToBottomNow();
			connect(panel, &QObject::destroyed, this, [this, panel]() {
				m_interactionPanels.removeAll(panel);
				});
		}
		else if (m_messages) {
			m_messages->addSystemMessage(QStringLiteral("收到提问请求，但无法创建内联面板。"), m_messagesLayout);
		}
	}
	else if (type == QStringLiteral("approval/requested")) {
		QWidget* panel = InteractionHandler::handleApproval(frame, m_api, m_messagesLayout);
		if (panel) {
			m_interactionPanels.append(panel);
			scrollToBottomNow();
			connect(panel, &QObject::destroyed, this, [this, panel]() {
				m_interactionPanels.removeAll(panel);
				});
		}
		else if (m_messages) {
			m_messages->addSystemMessage(QStringLiteral("收到审批请求，但无法创建内联面板。"), m_messagesLayout);
		}
	}
}

void DSHHub::handleTransportError(const QString& context, const QString& message)
{
	// 服务端主动重启时，旧 WebSocket 断开是预期行为，不当作错误刷到聊天区
	if (m_serverManager && m_serverManager->isRestarting())
		return;

	m_messages->addSystemMessage(
		QStringLiteral("传输错误 [%1]: %2").arg(context, message),
		m_messagesLayout);
}

void DSHHub::showNoMoreToast()
{
	if (!m_toastLabel)
		return;

	QWidget* parent = qobject_cast<QWidget*>(m_toastLabel->parent());
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

	auto* effect = new QGraphicsOpacityEffect(m_toastLabel);
	m_toastLabel->setGraphicsEffect(effect);

	auto* fadeIn = new QPropertyAnimation(effect, "opacity", m_toastLabel);
	fadeIn->setDuration(180);
	fadeIn->setStartValue(0.0);
	fadeIn->setEndValue(1.0);
	connect(fadeIn, &QPropertyAnimation::finished, this, [this]() {
		QTimer::singleShot(1200, this, [this]() {
			if (!m_toastLabel)
				return;

			auto* effect = qobject_cast<QGraphicsOpacityEffect*>(m_toastLabel->graphicsEffect());
			if (!effect)
				return;

			auto* fadeOut = new QPropertyAnimation(effect, "opacity", m_toastLabel);
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