#pragma once

#include "AgentPresetService.h"
#include "CredentialsService.h"
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
class QLineEdit;
class QListWidget;
class QTimer;
class DshApiClient;

// ------------------------------------------------------------------
// Settings —— “设置系统”整体类（不是一次性的窗口实例）
// ------------------------------------------------------------------
// Settings 是随主窗口（DSHHub）创建后一直存在的常驻对象，负责：
//   1) 设置界面的搭建与交互（API Key、Agent 预设、Server 地址、外观）；
//   2) 设置窗口本身的开关管理：灰色遮罩、居中、判重、关闭清理。
//
// 与设置相关的“功能逻辑”不在这里：
//   - 凭据文件/credentials.set      -> CredentialsService   (common)
//   - Agent 预设拉取/解析/选中决策  -> AgentPresetService   (common)
//   - QSettings 键的读写            -> SettingsStore        (common)
//
// 打开/关闭窗口统一走 openSettings() / closeSettings()：
//   - 主窗口只保留少量调用（例如把侧边栏的 settingsRequested
//     信号直接连到 openSettings()），不再在 DSHHub 里管理遮罩成员；
//   - 遮罩是 Settings 自己创建的子控件，随宿主窗口 resize 时通过
//     syncOverlayToHost() 保持铺满（由主窗口 resizeEvent 调用）；
//   - 右上角关闭按钮 / ESC 关闭时也会自动清理遮罩（PopupWindow
//     的 closed 信号 -> closeSettings()）。
// ------------------------------------------------------------------
class Settings : public PopupWindow
{
	Q_OBJECT

public:
	// dshHome：DSH 服务端 home（credentials 等文件所在目录）
	// api    ：DshApiClient，用于读取/保存 API Key 与 Agent 预设
	// host   ：宿主主窗口（用于定位遮罩与居中）
	explicit Settings(const QString& dshHome, DshApiClient* api, QWidget* host);

	// 打开设置窗口（幂等：已打开则直接返回；每次打开前刷新数据）
	void openSettings();

	// 关闭设置窗口并清理遮罩（幂等）
	void closeSettings();

	// 宿主窗口 resize 后调用，让遮罩重新铺满宿主（未打开时为空操作）
	void syncOverlayToHost();

signals:
	// API Key 通过 DSH credentials.set 保存失败时发出，宿主可用作重启兜底
	void apiKeyChanged();

	// 用户选择了一个 Agent 预设
	void agentPresetChanged(const QString& presetId);

	// 用户保存了服务器设置，宿主可以据此重启/重连
	void serverSettingsSaved();

private:
	// 把拉取到的预设填进下拉列表，并把选中项显示到按钮上
	void populateAgentPresets(const QVector<AgentPreset>& presets);
	void loadAgentPresets();
	void saveApiKeyToServer();
	void saveServerSettings();

	// 打开前刷新需要每次同步的数据（API Key、Server 地址、预设列表）
	void refreshOnOpen();

	CredentialsService m_credentials;
	QString m_pendingApiKey;
	DshApiClient* m_api = nullptr;
	QWidget* m_host = nullptr;            // 宿主主窗口（遮罩/居中定位）
	QWidget* m_overlay = nullptr;         // 本系统自管的灰色遮罩
	QTimer* m_apiKeyTimer = nullptr;
	QLineEdit* m_apiKeyEdit = nullptr;
	QLineEdit* m_serverUrlEdit = nullptr;
	QPushButton* m_agentPresetButton = nullptr;
	QFrame* m_agentPresetPopup = nullptr;
	QListWidget* m_agentPresetList = nullptr;
	QString m_serverUrlText;
};
