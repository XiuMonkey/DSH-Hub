#pragma once

#include "common/session/AgentPresetService.h"
#include "ui/PopupWindow.h"

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
class DshApiClient;
class ModelListPanel;

// "设置系统"整体类：与 PluginsManager 同思路的常驻对象，管设置界面的搭建与交互（模型列表、API Key、
// Agent 预设、Server 地址、外观）以及窗口本身的开关（遮罩、居中、判重、关闭清理）。功能逻辑不在这里：
// Agent 预设 -> AgentPresetService，模型目录读写 -> ModelSelectionService，QSettings ->
// SettingsStore，模型列表 UI -> ModelListPanel。
// 模型与凭据一律只与"当前所连服务端"打交道：客户端不做任何本地配置读写，也不针对具体提供方写死任何
// 东西（引用名由服务端的 profile 给出）。
class Settings : public PopupWindow
{
	Q_OBJECT

public:
	// api = DshApiClient（读写 Agent 预设、模型目录与凭据）；host = 宿主主窗口（用于定位遮罩与居中）
	explicit Settings(DshApiClient* api, QWidget* host);

	// 打开设置窗口（幂等：已打开则直接返回；每次打开前刷新数据）
	void openSettings();

	// 关闭设置窗口并归还遮罩（幂等）
	void closeSettings();

signals:
	// 用户把某个 Agent 预设设为默认且服务端已写入成功；生效范围由服务端定，只影响此后新建的会话
	void agentPresetChanged(const QString& presetId);

	// 服务端新增（或覆盖）了一个模型条目
	void modelAdded(const QString& provider, const QString& modelId);

	// 用户保存了服务器设置，宿主可以据此重启/重连
	void serverSettingsSaved();

private:
	// 把拉取到的预设填进下拉列表，并把选中项显示到按钮上
	void populateAgentPresets(const QVector<AgentPreset>& presets);
	void loadAgentPresets();
	void saveServerSettings();

	// 打开前刷新需要每次同步的数据（Server 地址、预设列表、模型列表）
	void refreshOnOpen() override;

	DshApiClient* m_api = nullptr;
	QLineEdit* m_serverUrlEdit = nullptr;
	ModelListPanel* m_modelList = nullptr;
	QPushButton* m_agentPresetButton = nullptr;
	QFrame* m_agentPresetPopup = nullptr;
	QListWidget* m_agentPresetList = nullptr;
	QString m_serverUrlText;
};
