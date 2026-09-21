#include "ui/Settings.h"
#include "common/appearance/ThemeManager.h"
#include "common/appearance/WindowFrame.h"
#include "network/DshApiClient.h"
#include "ui/ModelListPanel.h"
#include "common/settings/SettingsStore.h"
#include "common/appearance/TranslationManager.h"

#include <QComboBox>
#include <QDebug>
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
	setTitle(qtTrId("settings_title"));

	auto* content = new QWidget(this);
	auto* layout = new QHBoxLayout(content);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(12);

	auto* navLayout = new QVBoxLayout;
	navLayout->setContentsMargins(0, 0, 0, 0);
	navLayout->setSpacing(4);

	auto* modelButton = new SettingsButton(qtTrId("model_list_title"), content);
	auto* agentButton = new SettingsButton(qtTrId("settings_agent_preset_tab"), content);
	auto* serverButton = new SettingsButton(qtTrId("settings_server_tab"), content);
	auto* appearanceButton = new SettingsButton(qtTrId("settings_appearance_tab"), content);

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

	auto* agentLabel = new QLabel(qtTrId("settings_default_agent_preset"), agentPanel);
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
	ThemeManager::instance().repolishScrollArea(m_agentPresetList);
	popupLayout->addWidget(m_agentPresetList);

	auto* agentHint = new QLabel(qtTrId("settings_agent_preset_hint"), agentPanel);
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

		if (m_agentPresetPopup)
			m_agentPresetPopup->hide();

		// 先把按钮文字改成本次点的那个（乐观显示），写入失败再退回去 ——
		// 服务端没收下就不能假装改了。
		const QString previousText = m_agentPresetButton ? m_agentPresetButton->text() : QString();
		if (m_agentPresetButton) {
			m_agentPresetButton->setText(item->text());
			m_agentPresetButton->setToolTip(QString());
		}

		// “设为默认”= 一次服务端写入（settings/update，"agent-presets" + {default: id}），
		// 与原版 web 客户端同一条通道、同一个字段。客户端不再存本地副本：
		// 服务端 agentPresets/list 每行的 isDefault 就是权威显示来源。
		//
		// 生效范围由服务端决定，只影响**此后新建**的会话；已有会话各按自己
		// 日志里的记录跑，不会被回头改写 —— 所以这里不碰当前会话、也不刷新工具过滤。
		AgentPresetService::persistDefault(m_api, presetId,
			[this, presetId](const QString& savedId) {
				emit agentPresetChanged(savedId);
			},
			[this, previousText](const DshApiClient::RpcError& error) {
				qWarning().noquote() << QStringLiteral("[Settings] default agent preset write failed:")
					<< error.code << error.message;
				if (m_agentPresetButton) {
					m_agentPresetButton->setText(previousText);
					m_agentPresetButton->setToolTip(
						qtTrId("common_load_failed_fmt").arg(error.code, error.message));
				}
			});
		});
	// ---------------- Server 设置 ----------------
	auto* serverPanel = new QWidget(content);
	auto* serverLayout = new QVBoxLayout(serverPanel);
	serverLayout->setContentsMargins(0, 0, 0, 0);
	serverLayout->setSpacing(6);

	auto* serverLabel = new QLabel(qtTrId("settings_server_address"), serverPanel);
	serverLabel->setObjectName(QStringLiteral("settingsServerLabel"));

	m_serverUrlEdit = new QLineEdit(serverPanel);
	m_serverUrlEdit->setObjectName(QStringLiteral("serverUrlEdit"));
	m_serverUrlEdit->setPlaceholderText(QStringLiteral("http://127.0.0.1:3080"));
	// 初始文本：openSettings() 每次打开时经 refreshOnOpen() 同步

	auto* serverHint = new QLabel(qtTrId("settings_server_address_hint"), serverPanel);
	serverHint->setWordWrap(true);
	serverHint->setObjectName(QStringLiteral("settingsServerHint"));

	auto* serverSaveButton = new QPushButton(qtTrId("settings_save_and_restart"), serverPanel);
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
		qtTrId("settings_appearance_desc"),
		appearancePanel);
	appearanceHint->setWordWrap(true);
	appearanceHint->setObjectName(QStringLiteral("settingsAppearanceHint"));

	auto* stylesResetButton = new QPushButton(qtTrId("settings_reset_styles"), appearancePanel);
	stylesResetButton->setCursor(Qt::PointingHandCursor);
	stylesResetButton->setObjectName(QStringLiteral("settingsStylesResetButton"));

	auto* appearanceFeedback = new QLabel(appearancePanel);
	appearanceFeedback->setWordWrap(true);
	appearanceFeedback->setObjectName(QStringLiteral("settingsAppearanceFeedback"));
	appearanceFeedback->hide();

	connect(stylesResetButton, &QPushButton::clicked, this, [appearanceFeedback]() {
		ThemeManager::instance().resetStyles();
		if (appearanceFeedback) {
			appearanceFeedback->setText(
				qtTrId("settings_styles_reset_done"));
			appearanceFeedback->show();
		}
		});

	// ---------------- 界面语言 ----------------
	// 切换立即生效（免重启）：Translation::apply() 换掉 QTranslator 后 Qt 会给
	// 所有控件发 LanguageChange，各界面在自己的 changeEvent 里重设文案。
	auto* languageLabel = new QLabel(qtTrId("settings_language_label"), appearancePanel);
	languageLabel->setObjectName(QStringLiteral("settingsAppearanceLabel"));

	auto* languageCombo = new QComboBox(appearancePanel);
	languageCombo->setObjectName(QStringLiteral("settingsLanguageCombo"));
	languageCombo->setCursor(Qt::PointingHandCursor);

	// 第 0 项固定是“跟随系统”（数据为空串）
	languageCombo->addItem(qtTrId("settings_language_follow_system"), QString());
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
					languageFeedback->setText(qtTrId("settings_language_switched"));
					languageFeedback->show();
				}
				return;
			}

			// 对应语言包缺失：退回原文，并把原因说清楚（不是错误，只是没装语言包）
			if (languageFeedback) {
				languageFeedback->setText(qtTrId("settings_language_pack_missing_fmt")
					.arg(code.isEmpty() ? qtTrId("settings_language_system_name") : code));
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

	// 铺遮罩 + 居中 + 显示自己：背靠背完成，两者落在同一帧
	WindowFrame::showOverlayWithPopup(m_host, this, this);

	// 数据随后异步加载（API Key / Server 地址 / 预设列表）。刻意放在 show() 之后：
	// refreshOnOpen() 是本函数里最耗时的一步，放前面会让点击后"卡一下才出现"，
	// 放这里既不延迟弹窗出现，也不打断上面那两步的背靠背
	// （理由见 WindowFrame::showOverlayWithPopup）。
	refreshOnOpen();
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
	m_agentPresetButton->setText(qtTrId("common_loading"));

	AgentPresetService::fetch(m_api,
		[this](const QVector<AgentPreset>& presets) {
			populateAgentPresets(presets);
		},
		[this](const DshApiClient::RpcError& error) {
			m_agentPresetList->clear();
			m_agentPresetButton->setText(qtTrId("common_load_failed_fmt").arg(error.code, error.message));
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
		m_agentPresetButton->setText(qtTrId("settings_no_preset"));
		return;
	}

	// 选中项完全由服务端定：每行的 isDefault 就是 settings 文档里那个字段。
	// 客户端不存本地副本（写进去也是服务端），所以第一个参数传空串，
	// 让 resolveSelectedId 走“服务端默认 → 列表第一项”这条回落链。
	const QString selectedId = AgentPresetService::resolveSelectedId(presets, QString());

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