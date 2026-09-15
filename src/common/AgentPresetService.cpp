#include "AgentPresetService.h"

#include "SessionCommands.h"

// ------------------------------------------------------------------
// AgentPresetService.cpp
// ------------------------------------------------------------------
// 这里只放需要 DshApiClient 的联网部分；纯解析/决策逻辑是 inline 在头
// 文件里的（见 AgentPresetService.h），便于脱离网络做单元测试。
// ------------------------------------------------------------------

namespace AgentPresetService
{
	void fetch(
		DshApiClient* api,
		const std::function<void(const QVector<AgentPreset>& presets)>& onLoaded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError)
	{
		if (!api)
			return;

		// dsh 0.1.5：命名空间变成复数 agentPresets，且该端点无参数（args 为空）
		api->callMethod(
			QStringLiteral("agentPresets/list"),
			SessionCommands::emptyArgs(),
			[onLoaded](const QJsonObject& value) {
				if (onLoaded)
					onLoaded(parsePresets(value));
			},
			[onError](const DshApiClient::RpcError& error) {
				if (onError)
					onError(error);
			});
	}
}