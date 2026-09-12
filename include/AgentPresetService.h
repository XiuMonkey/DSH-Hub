#pragma once

// ------------------------------------------------------------------
// AgentPresetService.h
// ------------------------------------------------------------------
// Agent 预设的“功能逻辑”：
//   - 通过 agentPreset.list 拉取预设；
//   - 解析成结构化列表（id / 名称 / 是否服务端默认）；
//   - 结合本地记住的选择，算出“应该选中哪一个”。
// UI 只负责把结果填进列表控件并把选中项显示到按钮上。
// ------------------------------------------------------------------

#include "DshApiClient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <functional>

struct AgentPreset
{
	QString id;
	QString name;       // 服务端没给名字时回退为 id
	bool isDefault = false; // 服务端标记的默认预设
};

namespace AgentPresetService
{
	// 解析 agentPreset.list 的返回体（presets 数组）。
	// 纯函数且不依赖 DshApiClient，因此 inline 在头里（便于单测）。
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

	// 决定初始选中项：本地记住的 id 优先，其次服务端默认，最后退回第一项。
	// 列表为空时返回空串。
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

		// 记住的预设已不存在（或服务端没有默认项）→ 退回列表第一项
		return presets.first().id;
	}

	// 拉取预设列表
	void fetch(
		DshApiClient* api,
		const std::function<void(const QVector<AgentPreset>& presets)>& onLoaded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);
}
