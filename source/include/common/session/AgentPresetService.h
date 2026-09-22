#pragma once

// Agent 预设的功能逻辑：agentPresets/list 拉取并解析（id / 名称 / 是否服务端默认）、算出当前该显示哪一个、把某个预设写成“此后新建会话”的服务端默认。
// “默认预设”是**服务端设置字段**（settings/update ns = "agent-presets" 的 patch { default: <id> }，落盘 <DSH_HOME>/settings.yaml），客户端不保存任何本地副本。
// 生效范围：默认值只在新建会话时被解析成初值，已有会话的预设记录在各自会话日志里，服务端不回头改写。

#include "network/DshApiClient.h"

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
	// 承载“默认预设”的服务端设置命名空间（dsh-agent-presets 注册的那个）。
	inline constexpr const char* kSettingsNamespace = "agent-presets";

	// 写入默认预设用的 patch：只并这一个字段，不碰该命名空间的其它内容。
	inline QJsonObject defaultPatch(const QString& presetId)
	{
		QJsonObject patch;
		patch.insert(QStringLiteral("default"), presetId);
		return patch;
	}

	// 解析 agentPreset.list 的返回体（presets 数组）；纯函数且不依赖 DshApiClient，故 inline 在头里（便于单测）。
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

	// 初始选中项：本地记住的 id 优先，其次服务端默认，最后退回第一项；列表为空返回空串（本项目本地记忆已去掉、调用方传空串，故实际顺序就是“isDefault → 第一项”）。
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

	// 拉取预设列表
	void fetch(
		DshApiClient* api,
		const std::function<void(const QVector<AgentPreset>& presets)>& onLoaded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);

	// 把某个预设设为服务端默认；只影响此后新建的会话。服务端对该 namespace 只有 schema 约束（{ default: string }）、**不校验 id 是否存在**，故 presetId 必须来自 agentPresets/list 名单，否则之后每次新建会话都会以 agent-preset/not-found 失败；本函数只挡空串。
	void persistDefault(
		DshApiClient* api,
		const QString& presetId,
		const std::function<void(const QString& presetId)>& onSaved,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);
}
