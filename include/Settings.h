#pragma once

#include "PopupWindow.h"

#include <QPushButton>
#include <QString>

class SettingsButton : public QPushButton
{
	Q_OBJECT

public:
	explicit SettingsButton(const QString& text, QWidget* parent = nullptr);
};

class QFrame;
class QListWidget;
class QTimer;
class DshApiClient;

class Settings : public PopupWindow
{
	Q_OBJECT

public:
	explicit Settings(const QString& dshHome, DshApiClient* api, QWidget* parent = nullptr);

signals:
	// API Key 通过 DSH credentials.set 保存失败时发出，DSHHub 可用作重启兜底
	void apiKeyChanged();

	// 用户选择了一个 Agent 预设
	void agentPresetChanged(const QString& presetId);

	// 用户保存了服务器设置，DSHHub 可以据此重启/重连
	void serverSettingsSaved();

private:
	QString readApiKeyFromCredentialsFile() const;
	void writeApiKeyToCredentialsFile(const QString& apiKey);
	void saveApiKeyToServer();
	void loadAgentPresets();
	void saveServerSettings();

	QString m_dshHome;
	QString m_credentialsFile;
	QString m_pendingApiKey;
	DshApiClient* m_api = nullptr;
	QTimer* m_apiKeyTimer = nullptr;
	QPushButton* m_agentPresetButton = nullptr;
	QFrame* m_agentPresetPopup = nullptr;
	QListWidget* m_agentPresetList = nullptr;
	QString m_serverUrlText;
};
