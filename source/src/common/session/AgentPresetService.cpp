#include "common/session/AgentPresetService.h"

#include "common/session/SessionCommands.h"

// 这里只放需要 DshApiClient 的联网部分；纯解析/决策逻辑 inline 在头文件
// （见 AgentPresetService.h），便于脱离网络做单元测试。

namespace
{
	// dsh 0.1.5：斜杠 endpoint。
	const char* const kListMethod = "agentPresets/list";
	const char* const kSettingsUpdateMethod = "settings/update";
}

namespace AgentPresetService
{
	void fetch(DshApiClient* api, const std::function<void(const QVector<AgentPreset>& presets)>& onLoaded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError)
	{
		if (!api)
			return;

		// dsh 0.1.5：命名空间变成复数 agentPresets，且该端点无参数（args 为空）
		api->callMethod(QLatin1String(kListMethod), SessionCommands::emptyArgs(),
			[onLoaded](const QJsonObject& value) {
				if (onLoaded)
					onLoaded(parsePresets(value));
			},
			[onError](const DshApiClient::RpcError& error) {
				if (onError)
					onError(error);
			});
	}

	void persistDefault(DshApiClient* api, const QString& presetId,
		const std::function<void(const QString& presetId)>& onSaved,
		const std::function<void(const DshApiClient::RpcError& error)>& onError)
	{
		// 空 id 不能写：服务端不校验 id 是否存在，写进去会让之后每次建会话都以
		// agent-preset/not-found 失败（见头文件里的说明）。
		if (presetId.isEmpty()) {
			if (onError) {
				DshApiClient::RpcError error;
				error.code = QStringLiteral("invalid-request");
				error.message = qtTrId("settings_no_preset");
				onError(error);
			}
			return;
		}
		if (!api) {
			if (onError) {
				DshApiClient::RpcError error;
				error.code = QStringLiteral("invalid-request");
				error.message = qtTrId("settings_no_preset");
				onError(error);
			}
			return;
		}

		// settings/update 是把 patch 浅并进该命名空间 user 段，回包是该命名空间（已脱敏）的新视图；
		// 这里只需要"写成功"这个事实，不解析回包 —— 下一次 agentPresets/list 会带出新的 isDefault。
		api->callMethod(QLatin1String(kSettingsUpdateMethod),
			SessionCommands::settingsUpdate(QLatin1String(kSettingsNamespace), defaultPatch(presetId)),
			[presetId, onSaved](const QJsonObject&) {
				if (onSaved)
					onSaved(presetId);
			},
			[onError](const DshApiClient::RpcError& error) {
				if (onError)
					onError(error);
			});
	}
}
