#include "Settings.h"
#include "ThemeManager.h"
#include "DshApiClient.h"

#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonObject>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <QVBoxLayout>
#include <QTimer>
#include <QDebug>

SettingsButton::SettingsButton(QWidget* parent)
	: QPushButton(QStringLiteral("模型设置"), parent)
{
	setObjectName(QStringLiteral("modelSettingsButton"));
	setCursor(Qt::PointingHandCursor);
	setFixedWidth(120);
	setFixedHeight(36);
	setCheckable(true);
	setChecked(true);
	setStyleSheet(QStringLiteral("QPushButton#modelSettingsButton {") + QStringLiteral("  background: transparent;") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("  padding: 8px 16px;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("  font-size: 14px;") + QStringLiteral("  text-align: left;") + QStringLiteral("}") + QStringLiteral("QPushButton#modelSettingsButton:hover,") + QStringLiteral("QPushButton#modelSettingsButton:checked {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg")) + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QPushButton#modelSettingsButton:pressed {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";") + QStringLiteral("}"));
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

	auto* modelButton = new SettingsButton(content);
	layout->addWidget(modelButton, 0, Qt::AlignTop);

	auto* rightPanel = new QWidget(content);
	auto* rightLayout = new QVBoxLayout(rightPanel);
	rightLayout->setContentsMargins(0, 0, 0, 0);
	rightLayout->setSpacing(6);

	auto* apiLabel = new QLabel(QStringLiteral("API Key:"), rightPanel);
	apiLabel->setStyleSheet(QStringLiteral("QLabel {") + QStringLiteral("  background: transparent;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textSecondary")) + QStringLiteral(";") + QStringLiteral("  font-size: 12px;") + QStringLiteral("}"));

	auto* modelEdit = new QLineEdit(rightPanel);
	modelEdit->setObjectName(QStringLiteral("modelSettingsEdit"));
	modelEdit->setPlaceholderText(QStringLiteral("输入模型设置"));
	modelEdit->setText(readApiKeyFromCredentialsFile());
	modelEdit->setStyleSheet(QStringLiteral("QLineEdit#modelSettingsEdit {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("inputBg")) + QStringLiteral(";") + QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("scrollbar")) + QStringLiteral(";") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("  padding: 8px 12px;") + QStringLiteral("  font-size: 14px;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("  selection-background-color: ") + Theme::color(QStringLiteral("selectionBg")) + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QLineEdit#modelSettingsEdit:focus {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border-color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";") + QStringLiteral("}"));

	rightLayout->addWidget(apiLabel);
	rightLayout->addWidget(modelEdit);
	rightLayout->addStretch(1);

	layout->addWidget(rightPanel, 1);

	connect(modelButton, &QPushButton::toggled, rightPanel, &QWidget::setVisible);
	// 点击后保持选中状态，避免再次点击取消选中
	connect(modelButton, &QPushButton::clicked, modelButton, [modelButton]() {
		modelButton->setChecked(true);
		});

	// API Key 输入停止后，通过 DSH credentials.set 热更新凭据，避免重启
	m_apiKeyTimer = new QTimer(this);
	m_apiKeyTimer->setSingleShot(true);
	m_apiKeyTimer->setInterval(1200);
	connect(m_apiKeyTimer, &QTimer::timeout, this, &Settings::saveApiKeyToServer);

	connect(modelEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
		m_pendingApiKey = text;
		if (m_apiKeyTimer)
			m_apiKeyTimer->start();
		});

	setContent(content);

	resize(640, 480);
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