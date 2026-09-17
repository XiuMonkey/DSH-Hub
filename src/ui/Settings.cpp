#include "Settings.h"
#include "ThemeManager.h"
#include "WindowFrame.h"
#include "DshApiClient.h"
#include "ModelListPanel.h"
#include "SettingsStore.h"
#include "TranslationManager.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

SettingsButton::SettingsButton(const QString& text, QWidget* parent)
	: QPushButton(text, parent)
{
	setObjectName(QStringLiteral("settingsNavButton"));
	setCursor(Qt::PointingHandCursor);
	setFixedWidth(120);
	// 只给下限，不锁死高度：中文在两行高的字体度量下会顶到 36px 的上下边，
	// 固定高度就会把字裁掉。让按钮按自身 sizeHint（字高 + QSS padding）决定高度。
	setMinimumHeight(36);
	setCheckable(true);
}

Settings::Settings(DshApiClient* api, QWidget* host)
	: PopupWindow(host)
	, m_api(api)
	, m_host(host)
{
	setTitle(tr("设置"));

	auto* content = new QWidget(this);
	auto* layout = new QHBoxLayout(content);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(12);

	auto* navLayout = new QVBoxLayout;
	navLayout->setContentsMargins(0, 0, 0, 0);
	navLayout->setSpacing(4);

	auto* modelButton = new SettingsButton(tr("模型列表"), content);
	auto* agentButton = new SettingsButton(tr("Agent预设"), content);
	auto* serverButton = new SettingsButton(tr("Server设置"), content);
	auto* appearanceButton = new SettingsButton(tr("外观设置"), content);

	navLayout->addWidget(modelButton);
	navLayout->addWidget(agentButton);
	navLayout->addWidget(serverButton);
	navLayout->addWidget(appearanceButton);
	navLayout->addStretch(1);

	layout->addLayout(navLayout);

	// ---------------- 模型列表 ----------------
	// 模型信息与凭据的事实来源都在服务端：目录由适配器公布（session/modelCatalog），
	// 新增的条目写进 settings 文档，API Key 按该路由 profile 的 apiKeyEnv 引用
	// 经 credentials/set 写入。面板自己不存任何清单，打开即重新拉取。
	auto* modelPanel = new QWidget(content);
	auto* modelLayout = new QVBoxLayout(modelPanel);
	modelLayout->setContentsMargins(0, 0, 0, 0);
	modelLayout->setSpacing(6);

	m_modelList = new ModelListPanel(m_api, modelPanel);
	// 新增模型后通知宿主（宿主要刷新输入框底的模型选择器）
	connect(m_modelList, &ModelListPanel::modelAdded, this,
		[this](const QString& provider, const QString& modelId) {
			emit modelAdded(provider, modelId);
		});

	modelLayout->addWidget(m_modelList, 1);

	// ---------------- Agent 预设 ----------------
	auto* agentPanel = new QWidget(content);
	auto* agentLayout = new QVBoxLayout(agentPanel);
	agentLayout->setContentsMargins(0, 0, 0, 0);
	agentLayout->setSpacing(6);

	auto* agentLabel = new QLabel(tr("默认 Agent 预设"), agentPanel);
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

	auto* agentHint = new QLabel(tr("新会话将使用该预设；修改后对当前会话也会立即生效。"), agentPanel);
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

	auto* serverLabel = new QLabel(tr("服务器地址"), serverPanel);
	serverLabel->setObjectName(QStringLiteral("settingsServerLabel"));

	m_serverUrlEdit = new QLineEdit(serverPanel);
	m_serverUrlEdit->setObjectName(QStringLiteral("serverUrlEdit"));
	m_serverUrlEdit->setPlaceholderText(QStringLiteral("http://127.0.0.1:3080"));
	// 初始文本：openSettings() 每次打开时经 refreshOnOpen() 同步

	auto* serverHint = new QLabel(tr("留空表示使用内置 DSH 服务；保存后需要重启服务生效。"), serverPanel);
	serverHint->setWordWrap(true);
	serverHint->setObjectName(QStringLiteral("settingsServerHint"));

	auto* serverSaveButton = new QPushButton(tr("保存并重启服务"), serverPanel);
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
		tr("主题颜色与界面样式来自程序目录 styles/ 下的模板"
			"（theme-*.json 色板与各 *.qss 规则）。手动改坏后可用下面的按钮恢复默认。"),
		appearancePanel);
	appearanceHint->setWordWrap(true);
	appearanceHint->setObjectName(QStringLiteral("settingsAppearanceHint"));

	auto* stylesResetButton = new QPushButton(tr("重置样式为默认"), appearancePanel);
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
				tr("已恢复默认样式：styles/ 模板已重新生成，界面已刷新。"));
			appearanceFeedback->show();
		}
		});

	// ---------------- 界面语言 ----------------
	// 切换立即生效（免重启）：Translation::apply() 换掉 QTranslator 后 Qt 会给
	// 所有控件发 LanguageChange，各界面在自己的 changeEvent 里重设文案。
	auto* languageLabel = new QLabel(tr("界面语言："), appearancePanel);
	languageLabel->setObjectName(QStringLiteral("settingsAppearanceLabel"));

	auto* languageCombo = new QComboBox(appearancePanel);
	languageCombo->setObjectName(QStringLiteral("settingsLanguageCombo"));
	languageCombo->setCursor(Qt::PointingHandCursor);

	// 第 0 项固定是“跟随系统”（数据为空串）
	languageCombo->addItem(tr("跟随系统"), QString());
	for (const LanguageInfo& language : Translation::availableLanguages()) {
		languageCombo->addItem(language.name, language.code);
	}

	// 选中当前生效的语言：保存的代码为空 = 跟随系统
	const QString savedCode = Translation::savedLanguageCode();
	{
		const int index = savedCode.isEmpty()
			? 0
			: languageCombo->findData(savedCode);
		languageCombo->setCurrentIndex(index >= 0 ? index : 0);
	}

	auto* languageFeedback = new QLabel(appearancePanel);
	languageFeedback->setWordWrap(true);
	languageFeedback->setObjectName(QStringLiteral("settingsAppearanceFeedback"));
	languageFeedback->hide();

	connect(languageCombo, &QComboBox::currentIndexChanged, this,
		[this, languageCombo, languageFeedback](int) {
			const QString code = languageCombo->currentData().toString();
			Translation::setSavedLanguageCode(code);

			if (Translation::apply(code)) {
				if (languageFeedback) {
					languageFeedback->setText(tr("界面语言已切换。"));
					languageFeedback->show();
				}
				return;
			}

			// 对应语言包缺失：退回原文，并把原因说清楚（不是错误，只是没装语言包）
			if (languageFeedback) {
				languageFeedback->setText(tr("没有找到 %1 的语言包，已回到中文原文。"
					"把 dshhub_%1.qm 放进程序目录的 translations/ 即可启用。")
					.arg(code.isEmpty() ? tr("系统语言") : code));
				languageFeedback->show();
			}
		});

	appearanceLayout->addWidget(appearanceHint);
	appearanceLayout->addWidget(languageLabel);
	appearanceLayout->addWidget(languageCombo);
	appearanceLayout->addWidget(languageFeedback);
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

	// 遮罩：宿主主窗口上那唯一一层半透明控件（铺满内容区、不含自绘标题栏，
	// 否则窗口按钮会被一起盖住点不动）。showOverlay() 里带一次同步重绘，
	// 所以下面直接 show() 自己就行，不会再出现"弹窗先出、遮罩后到"。
	WindowFrame::showOverlay(m_host, this);

	// 打开前刷新数据（API Key / Server 地址 / 预设列表）
	refreshOnOpen();

	// 居中于宿主并显示
	move(m_host->geometry().center() - rect().center());
	show();
	raise();
}

void Settings::closeSettings()
{
	// 先收遮罩、再隐藏自己：收遮罩那一步会同步重绘一次主窗口，两件事落在
	// 同一帧上。反过来（或让遮罩等下一帧重绘）观感就是"设置窗口没了、
	// 遮罩还留一拍"。
	WindowFrame::hideOverlay(m_host, this);
	// 隐藏自己（常驻：不销毁，等待下次打开）
	hide();
}

void Settings::syncOverlayToHost()
{
	WindowFrame::syncOverlay(m_host);
}

void Settings::refreshOnOpen()
{
	// Server 地址：跟随当前实际连接的 DSH 服务
	m_serverUrlText = m_api ? m_api->baseUrl().toString() : QString();
	if (m_serverUrlEdit)
		m_serverUrlEdit->setText(m_serverUrlText);

	// Agent 预设列表（异步加载，服务端未就绪时按钮显示“加载失败”）
	loadAgentPresets();

	// 模型列表：每次打开都重新向服务端要目录（可能刚在别处改过 settings）
	if (m_modelList)
		m_modelList->refresh();
}

void Settings::loadAgentPresets()
{
	if (!m_agentPresetList || !m_agentPresetButton || !m_api)
		return;

	m_agentPresetList->clear();
	m_agentPresetButton->setText(tr("加载中..."));

	AgentPresetService::fetch(m_api,
		[this](const QVector<AgentPreset>& presets) {
			populateAgentPresets(presets);
		},
		[this](const DshApiClient::RpcError& error) {
			m_agentPresetList->clear();
			m_agentPresetButton->setText(tr("加载失败：%1 %2").arg(error.code, error.message));
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
		m_agentPresetButton->setText(tr("（无可用预设）"));
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