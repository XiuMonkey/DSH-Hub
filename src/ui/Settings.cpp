#include "Settings.h"
#include "ThemeManager.h"
#include "DshApiClient.h"

#include <QFrame>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStringList>
#include <QVBoxLayout>
#include <QTimer>
#include <QDebug>

SettingsButton::SettingsButton(const QString& text, QWidget* parent)
	: QPushButton(text, parent)
{
	setObjectName(QStringLiteral("settingsNavButton"));
	setCursor(Qt::PointingHandCursor);
	setFixedWidth(120);
	setFixedHeight(36);
	setCheckable(true);
	setStyleSheet(
		QStringLiteral("QPushButton#settingsNavButton {")
		+ QStringLiteral("  background: transparent;")
		+ QStringLiteral("  border: none;")
		+ QStringLiteral("  border-radius: 8px;")
		+ QStringLiteral("  padding: 8px 16px;")
		+ QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";")
		+ QStringLiteral("  font-size: 14px;")
		+ QStringLiteral("  text-align: left;")
		+ QStringLiteral("}")
		+ QStringLiteral("QPushButton#settingsNavButton:hover,")
		+ QStringLiteral("QPushButton#settingsNavButton:checked {")
		+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg")) + QStringLiteral(";")
		+ QStringLiteral("}")
		+ QStringLiteral("QPushButton#settingsNavButton:pressed {")
		+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";")
		+ QStringLiteral("}"));
}

Settings::Settings(const QString& dshHome, DshApiClient* api, QWidget* parent)
	: PopupWindow(parent)
	, m_dshHome(dshHome)
	, m_credentialsFile(dshHome.isEmpty() ? QString() : dshHome + QStringLiteral("/.credentials.yaml"))
	, m_api(api)
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

	navLayout->addWidget(modelButton);
	navLayout->addWidget(agentButton);
	navLayout->addWidget(serverButton);
	navLayout->addStretch(1);

	layout->addLayout(navLayout);

	// ---------------- 模型设置 ----------------
	auto* modelPanel = new QWidget(content);
	auto* modelLayout = new QVBoxLayout(modelPanel);
	modelLayout->setContentsMargins(0, 0, 0, 0);
	modelLayout->setSpacing(6);

	auto* apiLabel = new QLabel(QStringLiteral("API Key:"), modelPanel);
	apiLabel->setStyleSheet(QStringLiteral("QLabel { background: transparent; color: ")
		+ Theme::color(QStringLiteral("textSecondary")) + QStringLiteral("; font-size: 12px; }"));

	auto* modelEdit = new QLineEdit(modelPanel);
	modelEdit->setObjectName(QStringLiteral("modelSettingsEdit"));
	modelEdit->setPlaceholderText(QStringLiteral("输入模型设置"));
	modelEdit->setText(readApiKeyFromCredentialsFile());
	modelEdit->setStyleSheet(QStringLiteral("QLineEdit#modelSettingsEdit { background: ")
		+ Theme::color(QStringLiteral("inputBg")) + QStringLiteral("; border: 1px solid ")
		+ Theme::color(QStringLiteral("scrollbar")) + QStringLiteral("; border-radius: 8px; padding: 8px 12px; font-size: 14px; color: ")
		+ Theme::color(QStringLiteral("textPrimary")) + QStringLiteral("; }")
		+ QStringLiteral("QLineEdit#modelSettingsEdit:focus { background: ")
		+ Theme::color(QStringLiteral("panelBg")) + QStringLiteral("; border-color: ")
		+ Theme::color(QStringLiteral("accent")) + QStringLiteral("; }"));

	modelLayout->addWidget(apiLabel);
	modelLayout->addWidget(modelEdit);
	modelLayout->addStretch(1);

	// ---------------- Agent 预设 ----------------
	auto* agentPanel = new QWidget(content);
	auto* agentLayout = new QVBoxLayout(agentPanel);
	agentLayout->setContentsMargins(0, 0, 0, 0);
	agentLayout->setSpacing(6);

	auto* agentLabel = new QLabel(QStringLiteral("默认 Agent 预设"), agentPanel);
	agentLabel->setStyleSheet(QStringLiteral("QLabel { background: transparent; color: ")
		+ Theme::color(QStringLiteral("textSecondary")) + QStringLiteral("; font-size: 12px; }"));

	m_agentPresetButton = new QPushButton(agentPanel);
	m_agentPresetButton->setObjectName(QStringLiteral("agentPresetButton"));
	m_agentPresetButton->setMinimumWidth(280);
	m_agentPresetButton->setCursor(Qt::PointingHandCursor);
	m_agentPresetButton->setStyleSheet(
		QStringLiteral("QPushButton#agentPresetButton {")
		+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";")
		+ QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";")
		+ QStringLiteral("  border-radius: 10px;")
		+ QStringLiteral("  padding: 8px 14px;")
		+ QStringLiteral("  font-size: 14px;")
		+ QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";")
		+ QStringLiteral("  text-align: left;")
		+ QStringLiteral("}")
		+ QStringLiteral("QPushButton#agentPresetButton:hover {")
		+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg")) + QStringLiteral(";")
		+ QStringLiteral("  border-color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";")
		+ QStringLiteral("}")
		+ QStringLiteral("QPushButton#agentPresetButton:pressed {")
		+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg")) + QStringLiteral(";")
		+ QStringLiteral("}"));

	// 用普通 QFrame 做下拉面板，直接像按钮/气泡一样用 QSS border-radius。
	// 因为它是 Settings 窗口的子控件，父窗口背景会填满圆角外部，不会出现独立 Popup 的直角矩形背景。
	m_agentPresetPopup = new QFrame(this);
	m_agentPresetPopup->setObjectName(QStringLiteral("agentPresetPopup"));
	m_agentPresetPopup->setAttribute(Qt::WA_StyledBackground, true);
	m_agentPresetPopup->setStyleSheet(
		QStringLiteral("QFrame#agentPresetPopup {")
		+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";")
		+ QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";")
		+ QStringLiteral("  border-radius: 10px;")
		+ QStringLiteral("}"));
	m_agentPresetPopup->hide();

	auto* popupLayout = new QVBoxLayout(m_agentPresetPopup);
	popupLayout->setContentsMargins(6, 6, 6, 6);
	popupLayout->setSpacing(0);

	m_agentPresetList = new QListWidget(m_agentPresetPopup);
	m_agentPresetList->setObjectName(QStringLiteral("agentPresetList"));
	m_agentPresetList->setFrameShape(QFrame::NoFrame);
	m_agentPresetList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_agentPresetList->setStyleSheet(
		QStringLiteral("QListWidget#agentPresetList {")
		+ QStringLiteral("  background: transparent;")
		+ QStringLiteral("  border: none;")
		+ QStringLiteral("  outline: none;")
		+ QStringLiteral("}")
		+ QStringLiteral("QListWidget#agentPresetList::item {")
		+ QStringLiteral("  min-height: 34px;")
		+ QStringLiteral("  padding: 6px 12px;")
		+ QStringLiteral("  border-radius: 8px;")
		+ QStringLiteral("  margin: 2px 4px;")
		+ QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";")
		+ QStringLiteral("}")
		+ QStringLiteral("QListWidget#agentPresetList::item:hover {")
		+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg")) + QStringLiteral(";")
		+ QStringLiteral("}")
		+ QStringLiteral("QListWidget#agentPresetList::item:selected {")
		+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";")
		+ QStringLiteral("  color: ") + Theme::color(QStringLiteral("textOnAccent")) + QStringLiteral(";")
		+ QStringLiteral("}")
		+ QStringLiteral("QListWidget#agentPresetList QScrollBar:vertical {")
		+ QStringLiteral("  background: transparent;")
		+ QStringLiteral("  width: 8px;")
		+ QStringLiteral("  margin: 4px 2px;")
		+ QStringLiteral("}")
		+ QStringLiteral("QListWidget#agentPresetList QScrollBar::handle:vertical {")
		+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("scrollbar")) + QStringLiteral(";")
		+ QStringLiteral("  border-radius: 4px;")
		+ QStringLiteral("  min-height: 30px;")
		+ QStringLiteral("}")
		+ QStringLiteral("QListWidget#agentPresetList QScrollBar::handle:vertical:hover {")
		+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("scrollbarHover")) + QStringLiteral(";")
		+ QStringLiteral("}")
		+ QStringLiteral("QListWidget#agentPresetList QScrollBar::add-line:vertical,")
		+ QStringLiteral("QListWidget#agentPresetList QScrollBar::sub-line:vertical { height: 0; }")
		+ QStringLiteral("QListWidget#agentPresetList QScrollBar::add-page:vertical,")
		+ QStringLiteral("QListWidget#agentPresetList QScrollBar::sub-page:vertical { background: transparent; }"));
	popupLayout->addWidget(m_agentPresetList);

	auto* agentHint = new QLabel(QStringLiteral("新会话将使用该预设；修改后对当前会话也会立即生效。"), agentPanel);
	agentHint->setWordWrap(true);
	agentHint->setStyleSheet(QStringLiteral("QLabel { background: transparent; color: ")
		+ Theme::color(QStringLiteral("textSecondary")) + QStringLiteral("; font-size: 12px; }"));

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

		QSettings settings;
		settings.setValue(QStringLiteral("agent/defaultPreset"), presetId);
		emit agentPresetChanged(presetId);
		});
	// ---------------- Server 设置 ----------------
	auto* serverPanel = new QWidget(content);
	auto* serverLayout = new QVBoxLayout(serverPanel);
	serverLayout->setContentsMargins(0, 0, 0, 0);
	serverLayout->setSpacing(6);

	auto* serverLabel = new QLabel(QStringLiteral("服务器地址"), serverPanel);
	serverLabel->setStyleSheet(QStringLiteral("QLabel { background: transparent; color: ")
		+ Theme::color(QStringLiteral("textSecondary")) + QStringLiteral("; font-size: 12px; }"));

	auto* serverUrlEdit = new QLineEdit(serverPanel);
	serverUrlEdit->setObjectName(QStringLiteral("serverUrlEdit"));
	serverUrlEdit->setPlaceholderText(QStringLiteral("http://127.0.0.1:3080"));
	m_serverUrlText = m_api ? m_api->baseUrl().toString() : QString();
	serverUrlEdit->setText(m_serverUrlText);
	serverUrlEdit->setStyleSheet(QStringLiteral("QLineEdit#serverUrlEdit { background: ")
		+ Theme::color(QStringLiteral("inputBg")) + QStringLiteral("; border: 1px solid ")
		+ Theme::color(QStringLiteral("scrollbar")) + QStringLiteral("; border-radius: 8px; padding: 8px 12px; font-size: 14px; color: ")
		+ Theme::color(QStringLiteral("textPrimary")) + QStringLiteral("; }"));

	auto* serverHint = new QLabel(QStringLiteral("留空表示使用内置 DSH 服务；保存后需要重启服务生效。"), serverPanel);
	serverHint->setWordWrap(true);
	serverHint->setStyleSheet(QStringLiteral("QLabel { background: transparent; color: ")
		+ Theme::color(QStringLiteral("textSecondary")) + QStringLiteral("; font-size: 12px; }"));

	auto* serverSaveButton = new QPushButton(QStringLiteral("保存并重启服务"), serverPanel);
	serverSaveButton->setCursor(Qt::PointingHandCursor);
	serverSaveButton->setStyleSheet(QStringLiteral("QPushButton { background: ")
		+ Theme::color(QStringLiteral("accent")) + QStringLiteral("; color: ")
		+ Theme::color(QStringLiteral("textOnAccent")) + QStringLiteral("; border: none; border-radius: 8px; padding: 8px 14px; }"));

	serverLayout->addWidget(serverLabel);
	serverLayout->addWidget(serverUrlEdit);
	serverLayout->addWidget(serverHint);
	serverLayout->addWidget(serverSaveButton);
	serverLayout->addStretch(1);

	connect(serverUrlEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
		m_serverUrlText = text.trimmed();
		});
	connect(serverSaveButton, &QPushButton::clicked, this, &Settings::saveServerSettings);

	// ---------------- 栏目切换 ----------------
	layout->addWidget(modelPanel, 1);
	layout->addWidget(agentPanel, 1);
	layout->addWidget(serverPanel, 1);

	modelPanel->setVisible(true);
	agentPanel->setVisible(false);
	serverPanel->setVisible(false);
	modelButton->setChecked(true);

	const QList<QPushButton*> navButtons = { modelButton, agentButton, serverButton };
	const QList<QWidget*> navPanels = { modelPanel, agentPanel, serverPanel };

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

	connect(modelEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
		m_pendingApiKey = text;
		if (m_apiKeyTimer)
			m_apiKeyTimer->start();
		});

	setContent(content);

	resize(680, 480);

	loadAgentPresets();
}

QString Settings::readApiKeyFromCredentialsFile() const
{
	QFile file(m_credentialsFile);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return QString();

	QRegularExpression re(QStringLiteral("^\\s*DEEPSEEK_API_KEY\\s*:\\s*(.*)$"));
	while (!file.atEnd()) {
		const QString line = QString::fromUtf8(file.readLine()).trimmed();
		const auto match = re.match(line);
		if (match.hasMatch()) {
			QString value = match.captured(1).trimmed();
			if ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
				|| (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))) {
				value = value.mid(1, value.size() - 2);
			}
			return value;
		}
	}

	return QString();
}

void Settings::writeApiKeyToCredentialsFile(const QString& apiKey)
{
	if (m_credentialsFile.isEmpty())
		return;

	QDir().mkpath(m_dshHome);

	QStringList lines;
	QFile file(m_credentialsFile);
	if (file.exists()) {
		if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
			while (!file.atEnd())
				lines.append(QString::fromUtf8(file.readLine()));
			file.close();
		}
	}

	QRegularExpression re(QStringLiteral("^\\s*DEEPSEEK_API_KEY\\s*:.*$"));
	bool replaced = false;
	for (QString& line : lines) {
		if (re.match(line).hasMatch()) {
			line = QStringLiteral("DEEPSEEK_API_KEY: %1\n").arg(apiKey);
			replaced = true;
			break;
		}
	}
	if (!replaced)
		lines.append(QStringLiteral("DEEPSEEK_API_KEY: %1\n").arg(apiKey));

	QSaveFile out(m_credentialsFile);
	if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
		return;

	for (const QString& line : lines)
		out.write(line.toUtf8());

	if (out.commit()) {
		qInfo().noquote() << "[Settings] API key updated";
	}
	else {
		qWarning().noquote() << "[Settings] failed to commit credentials file: " << m_credentialsFile;
	}
}

void Settings::saveApiKeyToServer()
{
	if (m_api && !m_pendingApiKey.isEmpty()) {
		QJsonObject payload;
		payload.insert(QStringLiteral("ref"), QStringLiteral("DEEPSEEK_API_KEY"));
		payload.insert(QStringLiteral("value"), m_pendingApiKey);

		m_api->callMethod(
			QStringLiteral("credentials.set"),
			payload,
			[this](const QJsonObject&) {
				qInfo().noquote() << "[Settings] API key saved via credentials.set";
			},
			[this](const DshApiClient::RpcError& error) {
				qWarning().noquote() << "[Settings] credentials.set failed:"
					<< error.code << error.message;
				// 走老逻辑：直接写文件，并通知 DSHHub 重启兜底
				writeApiKeyToCredentialsFile(m_pendingApiKey);
				emit apiKeyChanged();
			});
	}
	else {
		writeApiKeyToCredentialsFile(m_pendingApiKey);
		emit apiKeyChanged();
	}
}

void Settings::loadAgentPresets()
{
	if (!m_agentPresetList || !m_agentPresetButton || !m_api)
		return;

	m_agentPresetList->clear();
	m_agentPresetButton->setText(QStringLiteral("加载中..."));

	m_api->callMethod(
		QStringLiteral("agentPreset.list"),
		{},
		[this](const QJsonObject& value) {
			const QJsonArray presets = value.value(QStringLiteral("presets")).toArray();
			QString defaultId;

			QSettings settings;
			const QString savedPreset = settings.value(QStringLiteral("agent/defaultPreset")).toString();

			m_agentPresetList->clear();
			for (const auto& value : presets) {
				const QJsonObject preset = value.toObject();
				const QString id = preset.value(QStringLiteral("id")).toString();
				if (id.isEmpty())
					continue;

				QString name = preset.value(QStringLiteral("name")).toString();
				if (name.isEmpty())
					name = id;
				if (preset.value(QStringLiteral("isDefault")).toBool())
					defaultId = id;

				auto* item = new QListWidgetItem(name, m_agentPresetList);
				item->setData(Qt::UserRole, id);
			}

			if (m_agentPresetList->count() == 0) {
				m_agentPresetButton->setText(QStringLiteral("（无可用预设）"));
				return;
			}

			const QString selectedId = savedPreset.isEmpty() ? defaultId : savedPreset;
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
				selectedName = m_agentPresetList->item(0)->text();
				m_agentPresetList->item(0)->setSelected(true);
			}

			m_agentPresetButton->setText(selectedName);

			// 根据预设数量调整下拉面板高度
			const int itemHeight = 38;
			const int maxHeight = 320;
			const int height = qMin(maxHeight, m_agentPresetList->count() * itemHeight + 12);
			if (m_agentPresetPopup)
				m_agentPresetPopup->setFixedHeight(height);
		},
		[this](const DshApiClient::RpcError& error) {
			m_agentPresetList->clear();
			m_agentPresetButton->setText(QStringLiteral("加载失败：%1 %2").arg(error.code, error.message));
		});
}
void Settings::saveServerSettings()
{
	QSettings settings;
	const QString url = m_serverUrlText.trimmed();
	if (url.isEmpty()) {
		settings.remove(QStringLiteral("server/url"));
	}
	else {
		settings.setValue(QStringLiteral("server/url"), url);
	}

	emit serverSettingsSaved();
}