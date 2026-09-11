#include "Settings.h"
#include "ThemeManager.h"
#include "WindowFrame.h"
#include "DshApiClient.h"
#include "SettingsStore.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

SettingsButton::SettingsButton(const QString& text, QWidget* parent)
	: QPushButton(text, parent)
{
	setObjectName(QStringLiteral("settingsNavButton"));
	setCursor(Qt::PointingHandCursor);
	setFixedWidth(120);
	setFixedHeight(36);
	setCheckable(true);
}

Settings::Settings(const QString& dshHome, DshApiClient* api, QWidget* host)
	: PopupWindow(host)
	, m_credentials(dshHome)
	, m_api(api)
	, m_host(host)
{
	setTitle(QStringLiteral("设置"));

	auto* content = new QWidget(this);
	auto* layout = new QHBoxLayout(content);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(12);

	auto* navLayout = new QVBoxLayout;
	navLayout->setContentsMargins(0, 0, 0, 0);
	navLayout->setSpacing(4);

	auto* modelButton = new SettingsButton(QStringLiteral("模型设置"), content);
	auto* agentButton = new SettingsButton(QStringLiteral("Agent预设"), content);
	auto* serverButton = new SettingsButton(QStringLiteral("Server设置"), content);
	auto* appearanceButton = new SettingsButton(QStringLiteral("外观设置"), content);

	navLayout->addWidget(modelButton);
	navLayout->addWidget(agentButton);
	navLayout->addWidget(serverButton);
	navLayout->addWidget(appearanceButton);
	navLayout->addStretch(1);

	layout->addLayout(navLayout);

	// ---------------- 模型设置 ----------------
	auto* modelPanel = new QWidget(content);
	auto* modelLayout = new QVBoxLayout(modelPanel);
	modelLayout->setContentsMargins(0, 0, 0, 0);
	modelLayout->setSpacing(6);

	auto* apiLabel = new QLabel(QStringLiteral("API Key:"), modelPanel);
	apiLabel->setObjectName(QStringLiteral("settingsApiLabel"));

	m_apiKeyEdit = new QLineEdit(modelPanel);
	m_apiKeyEdit->setObjectName(QStringLiteral("modelSettingsEdit"));
	m_apiKeyEdit->setPlaceholderText(QStringLiteral("输入模型设置"));
	// 初始文本：openSettings() 每次打开时经 refreshOnOpen() 同步。
	// 避免常驻对象在服务端未就绪时就读取凭据。

	modelLayout->addWidget(apiLabel);
	modelLayout->addWidget(m_apiKeyEdit);
	modelLayout->addStretch(1);

	// ---------------- Agent 预设 ----------------
	auto* agentPanel = new QWidget(content);
	auto* agentLayout = new QVBoxLayout(agentPanel);
	agentLayout->setContentsMargins(0, 0, 0, 0);
	agentLayout->setSpacing(6);

	auto* agentLabel = new QLabel(QStringLiteral("默认 Agent 预设"), agentPanel);
	agentLabel->setObjectName(QStringLiteral("settingsAgentLabel"));

	m_agentPresetButton = new QPushButton(agentPanel);
	m_agentPresetButton->setObjectName(QStringLiteral("agentPresetButton"));
	m_agentPresetButton->setMinimumWidth(280);
	m_agentPresetButton->setCursor(Qt::PointingHandCursor);

	// 用普通 QFrame 做下拉面板，直接像按钮气泡一样用 QSS border-radius。
	// 因为它是 Settings 窗口的子控件，父窗口背景会填满圆角外部，不会出现独立 Popup 的直角矩形背景。
	m_agentPresetPopup = new QFrame(this);
	m_agentPresetPopup->setObjectName(QStringLiteral("agentPresetPopup"));
	m_agentPresetPopup->setAttribute(Qt::WA_StyledBackground, true);
	m_agentPresetPopup->hide();

	auto* popupLayout = new QVBoxLayout(m_agentPresetPopup);
	popupLayout->setContentsMargins(6, 6, 6, 6);
	popupLayout->setSpacing(0);

	m_agentPresetList = new QListWidget(m_agentPresetPopup);
	m_agentPresetList->setObjectName(QStringLiteral("agentPresetList"));
	m_agentPresetList->setFrameShape(QFrame::NoFrame);
	m_agentPresetList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	// 滚动条早于 objectName 存在（基类构造时创建），设完名字重新解析一次，
	// 否则 #agentPresetList QScrollBar 规则匹配不上、滚动条按原生样式画
	Theme::repolishScrollArea(m_agentPresetList);
	popupLayout->addWidget(m_agentPresetList);

	auto* agentHint = new QLabel(QStringLiteral("新会话将使用该预设；修改后对当前会话也会立即生效。"), agentPanel);
	agentHint->setWordWrap(true);
	agentHint->setObjectName(QStringLiteral("settingsAgentHint"));

	agentLayout->addWidget(agentLabel);
	agentLayout->addWidget(m_agentPresetButton);
	agentLayout->addWidget(agentHint);
	agentLayout->addStretch(1);

	connect(m_agentPresetButton, &QPushButton::clicked, this, [this]() {
		if (!m_agentPresetPopup || !m_agentPresetButton)
			return;

		if (m_agentPresetPopup->isVisible()) {
			m_agentPresetPopup->hide();
			return;
		}

		m_agentPresetPopup->setFixedWidth(m_agentPresetButton->width());
		m_agentPresetPopup->move(m_agentPresetButton->mapTo(this, QPoint(0, m_agentPresetButton->height() + 4)));
		m_agentPresetPopup->show();
		m_agentPresetPopup->raise();
		});

	connect(m_agentPresetList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
		if (!item)
			return;

		const QString presetId = item->data(Qt::UserRole).toString();
		if (presetId.isEmpty())
			return;

		if (m_agentPresetButton)
			m_agentPresetButton->setText(item->text());
		if (m_agentPresetPopup)
			m_agentPresetPopup->hide();

		SettingsStore::setDefaultAgentPresetId(presetId);
		emit agentPresetChanged(presetId);
		});
	// ---------------- Server 设置 ----------------
	auto* serverPanel = new QWidget(content);
	auto* serverLayout = new QVBoxLayout(serverPanel);
	serverLayout->setContentsMargins(0, 0, 0, 0);
	serverLayout->setSpacing(6);

	auto* serverLabel = new QLabel(QStringLiteral("服务器地址"), serverPanel);
	serverLabel->setObjectName(QStringLiteral("settingsServerLabel"));

	m_serverUrlEdit = new QLineEdit(serverPanel);
	m_serverUrlEdit->setObjectName(QStringLiteral("serverUrlEdit"));
	m_serverUrlEdit->setPlaceholderText(QStringLiteral("http://127.0.0.1:3080"));
	// 初始文本：openSettings() 每次打开时经 refreshOnOpen() 同步

	auto* serverHint = new QLabel(QStringLiteral("留空表示使用内置 DSH 服务；保存后需要重启服务生效。"), serverPanel);
	serverHint->setWordWrap(true);
	serverHint->setObjectName(QStringLiteral("settingsServerHint"));

	auto* serverSaveButton = new QPushButton(QStringLiteral("保存并重启服务"), serverPanel);
	serverSaveButton->setCursor(Qt::PointingHandCursor);
	serverSaveButton->setObjectName(QStringLiteral("settingsServerSaveButton"));

	serverLayout->addWidget(serverLabel);
	serverLayout->addWidget(m_serverUrlEdit);
	serverLayout->addWidget(serverHint);
	serverLayout->addWidget(serverSaveButton);
	serverLayout->addStretch(1);

	connect(m_serverUrlEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
		m_serverUrlText = text.trimmed();
		});
	connect(serverSaveButton, &QPushButton::clicked, this, &Settings::saveServerSettings);

	// ---------------- 外观 ----------------
	auto* appearancePanel = new QWidget(content);
	auto* appearanceLayout = new QVBoxLayout(appearancePanel);
	appearanceLayout->setContentsMargins(0, 0, 0, 0);
	appearanceLayout->setSpacing(6);

	auto* appearanceHint = new QLabel(
		QStringLiteral("主题颜色与界面样式来自程序目录 styles/ 下的模板"
			"（theme-*.json 色板与各 *.qss 规则）。手动改坏后可用下面的按钮恢复默认。"),
		appearancePanel);
	appearanceHint->setWordWrap(true);
	appearanceHint->setObjectName(QStringLiteral("settingsAppearanceHint"));

	auto* stylesResetButton = new QPushButton(QStringLiteral("重置样式为默认"), appearancePanel);
	stylesResetButton->setCursor(Qt::PointingHandCursor);
	stylesResetButton->setObjectName(QStringLiteral("settingsStylesResetButton"));

	auto* appearanceFeedback = new QLabel(appearancePanel);
	appearanceFeedback->setWordWrap(true);
	appearanceFeedback->setObjectName(QStringLiteral("settingsAppearanceFeedback"));
	appearanceFeedback->hide();

	connect(stylesResetButton, &QPushButton::clicked, this, [appearanceFeedback]() {
		Theme::resetStyles();
		if (appearanceFeedback) {
			appearanceFeedback->setText(
				QStringLiteral("已恢复默认样式：styles/ 模板已重新生成，界面已刷新。"));
			appearanceFeedback->show();
		}
		});

	appearanceLayout->addWidget(appearanceHint);
	appearanceLayout->addWidget(stylesResetButton);
	appearanceLayout->addWidget(appearanceFeedback);
	appearanceLayout->addStretch(1);

	// ---------------- 栏目切换 ----------------
	layout->addWidget(modelPanel, 1);
	layout->addWidget(agentPanel, 1);
	layout->addWidget(serverPanel, 1);
	layout->addWidget(appearancePanel, 1);

	modelPanel->setVisible(true);
	agentPanel->setVisible(false);
	serverPanel->setVisible(false);
	appearancePanel->setVisible(false);
	modelButton->setChecked(true);

	const QList<QPushButton*> navButtons = { modelButton, agentButton, serverButton, appearanceButton };
	const QList<QWidget*> navPanels = { modelPanel, agentPanel, serverPanel, appearancePanel };

	for (int i = 0; i < navButtons.size(); ++i) {
		const int index = i;
		connect(navButtons.at(i), &QPushButton::clicked, this, [this, navButtons, navPanels, index]() {
			if (m_agentPresetPopup)
				m_agentPresetPopup->hide();
			for (int j = 0; j < navButtons.size(); ++j) {
				const bool active = (j == index);
				navButtons.at(j)->setChecked(active);
				navPanels.at(j)->setVisible(active);
			}
			});
	}

	// API Key 输入停止后，通过 DSH credentials.set 热更新凭据，避免重启
	m_apiKeyTimer = new QTimer(this);
	m_apiKeyTimer->setSingleShot(true);
	m_apiKeyTimer->setInterval(500);
	connect(m_apiKeyTimer, &QTimer::timeout, this, &Settings::saveApiKeyToServer);

	connect(m_apiKeyEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
		m_pendingApiKey = text;
		if (m_apiKeyTimer)
			m_apiKeyTimer->start();
		});

	// 常驻设置系统：窗口被关闭（右上角 ✕ / ESC）后自动清理遮罩并隐藏，
	// 对象本身不销毁，供下次 openSettings() 复用。
	connect(this, &PopupWindow::closed, this, &Settings::closeSettings);

	setContent(content);

	resize(680, 480);
	// 预设列表在每次打开时经 refreshOnOpen() 加载，这里不再预取。
	hide();
}

// ------------------------------------------------------------------
// 窗口开关管理（设置系统自管，不再由 DSHHub 代管）
// ------------------------------------------------------------------

void Settings::openSettings()
{
	if (!m_host)
		return;
	// 判重：窗口已经打开时忽略重复请求
	if (isVisible())
		return;

	// 遮罩：宿主主窗口的子控件，铺满内容区并盖住主界面
	// （不含自绘标题栏，否则窗口按钮会被一起盖住点不动）
	if (!m_overlay) {
		m_overlay = new QWidget(m_host);
		m_overlay->setObjectName(QStringLiteral("settingsOverlay"));
		m_overlay->setAttribute(Qt::WA_StyledBackground, true);
	}
	m_overlay->setGeometry(WindowFrame::overlayRect(m_host));
	m_overlay->raise();
	m_overlay->show();
	// 强制先让遮罩画出来（否则与下方弹窗同一帧才呈现，观感像弹窗先出、遮罩延迟）
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

	// 打开前刷新数据（API Key / Server 地址 / 预设列表）
	refreshOnOpen();

	// 居中于宿主并显示
	move(m_host->geometry().center() - rect().center());
	show();
	raise();
}

void Settings::closeSettings()
{
	// 关闭遮罩（常驻复用：只隐藏，不销毁，避免每次开关重新创建全窗半透明控件）
	if (m_overlay)
		m_overlay->hide();
	// 隐藏自己（常驻：不销毁，等待下次打开）
	hide();
}

void Settings::syncOverlayToHost()
{
	if (m_overlay && m_host)
		m_overlay->setGeometry(WindowFrame::overlayRect(m_host));
}

void Settings::refreshOnOpen()
{
	// API Key：每次打开时从凭据文件重新读取（可能与上次保存不同步）
	if (m_apiKeyEdit)
		m_apiKeyEdit->setText(m_credentials.readApiKey());

	// Server 地址：跟随当前实际连接的 DSH 服务
	m_serverUrlText = m_api ? m_api->baseUrl().toString() : QString();
	if (m_serverUrlEdit)
		m_serverUrlEdit->setText(m_serverUrlText);

	// Agent 预设列表（异步加载，服务端未就绪时按钮显示“加载失败”）
	loadAgentPresets();
}

void Settings::saveApiKeyToServer()
{
	// 服务端热更新优先，失败由 CredentialsService 回退写文件并回调告知
	m_credentials.saveApiKey(m_api, m_pendingApiKey, [this](bool usedFileFallback) {
		if (usedFileFallback)
			emit apiKeyChanged();
		});
}

void Settings::loadAgentPresets()
{
	if (!m_agentPresetList || !m_agentPresetButton || !m_api)
		return;

	m_agentPresetList->clear();
	m_agentPresetButton->setText(QStringLiteral("加载中..."));

	AgentPresetService::fetch(m_api,
		[this](const QVector<AgentPreset>& presets) {
			populateAgentPresets(presets);
		},
		[this](const DshApiClient::RpcError& error) {
			m_agentPresetList->clear();
			m_agentPresetButton->setText(QStringLiteral("加载失败：%1 %2").arg(error.code, error.message));
		});
}

void Settings::populateAgentPresets(const QVector<AgentPreset>& presets)
{
	if (!m_agentPresetList || !m_agentPresetButton)
		return;

	m_agentPresetList->clear();
	for (const AgentPreset& preset : presets) {
		auto* item = new QListWidgetItem(preset.name, m_agentPresetList);
		item->setData(Qt::UserRole, preset.id);
	}

	if (m_agentPresetList->count() == 0) {
		m_agentPresetButton->setText(QStringLiteral("（无可用预设）"));
		return;
	}

	// 本地记住的选择优先，其次服务端默认，最后退回第一个。
	const QString selectedId = AgentPresetService::resolveSelectedId(
		presets, SettingsStore::defaultAgentPresetId());

	QString selectedName;
	for (int i = 0; i < m_agentPresetList->count(); ++i) {
		QListWidgetItem* item = m_agentPresetList->item(i);
		if (item->data(Qt::UserRole).toString() == selectedId) {
			item->setSelected(true);
			selectedName = item->text();
			break;
		}
	}

	if (selectedName.isEmpty()) {
		m_agentPresetList->item(0)->setSelected(true);
		selectedName = m_agentPresetList->item(0)->text();
	}

	m_agentPresetButton->setText(selectedName);

	// 根据预设数量调整下拉面板高度
	const int itemHeight = 38;
	const int maxHeight = 320;
	const int height = qMin(maxHeight, m_agentPresetList->count() * itemHeight + 12);
	if (m_agentPresetPopup)
		m_agentPresetPopup->setFixedHeight(height);
}

void Settings::saveServerSettings()
{
	SettingsStore::setServerUrl(m_serverUrlText);
	emit serverSettingsSaved();
}