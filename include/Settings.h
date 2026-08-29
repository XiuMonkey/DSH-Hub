#pragma once

#include "PopupWindow.h"

#include <QPushButton>
#include <QString>

class SettingsButton : public QPushButton
{
	Q_OBJECT

public:
	explicit SettingsButton(QWidget* parent = nullptr);
};

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

private:
	QString readApiKeyFromCredentialsFile() const;
	void writeApiKeyToCredentialsFile(const QString& apiKey);
	void saveApiKeyToServer();

	QString m_dshHome;
	QString m_credentialsFile;
	QString m_pendingApiKey;
	DshApiClient* m_api = nullptr;
	QTimer* m_apiKeyTimer = nullptr;
};
