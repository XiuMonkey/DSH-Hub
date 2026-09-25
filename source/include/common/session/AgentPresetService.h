#pragma once

// Agent 预设的功能逻辑：拉取 agentPresets/list、算出当前该显示哪一个、标记"此后新建会话"的服务端默认。
// 默认预设存在服务端 settings（ns = "agent-presets" 的 patch { default: <id> }），客户端不留副本。

#include "network/DshApiClient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <functional>

struct AgentPreset
{
	QString id;
	QString name;
	bool isDefault = false;
};

namespace AgentPresetService
{
	// "默认预设"所在的服务端设置命名空间
	inline constexpr const char* kSettingsNamespace = "agent-presets";

	inline QJsonObject defaultPatch(const QString& presetId)
	{
		QJsonObject patch;
		patch.insert(QStringLiteral("default"), presetId);
		return patch;
	}

	// 纯函数，故 inline 在头里便于单测
	inline QVector<AgentPreset> parsePresets(const QJsonObject& value)
	{
		QVector<AgentPreset> presets;
		const QJsonArray array = value.value(QStringLiteral("presets")).toArray();
		presets.reserve(array.size());

		for (const auto& item : array) {
			const QJsonObject preset = item.toObject();

			AgentPreset parsed;
			parsed.id = preset.value(QStringLiteral("id")).toString();
			if (parsed.id.isEmpty())
				continue;

			parsed.name = preset.value(QStringLiteral("name")).toString();
			if (parsed.name.isEmpty())
				parsed.name = parsed.id;

			parsed.isDefault = preset.value(QStringLiteral("isDefault")).toBool();
			presets.append(parsed);
		}

		return presets;
	}

	// 初始选中项：本地 id → 服务端默认 → 第一项
	inline QString resolveSelectedId(const QVector<AgentPreset>& presets, const QString& savedPresetId)
	{
		if (presets.isEmpty())
			return QString();

		QString serverDefaultId;
		for (const AgentPreset& preset : presets) {
			if (preset.isDefault) {
				serverDefaultId = preset.id;
				break;
			}
		}

		const QString wanted = savedPresetId.isEmpty() ? serverDefaultId : savedPresetId;
		for (const AgentPreset& preset : presets) {
			if (preset.id == wanted)
				return preset.id;
		}

		return presets.first().id;
	}

	void fetch(DshApiClient* api, const std::function<void(const QVector<AgentPreset>& presets)>& onLoaded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);

	// ⚠️ 服务端不校验 id 存在，presetId 必须来自 agentPresets/list 名单，否则新建会话 not-found
	void persistDefault(DshApiClient* api, const QString& presetId,
		const std::function<void(const QString& presetId)>& onSaved,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);
}
