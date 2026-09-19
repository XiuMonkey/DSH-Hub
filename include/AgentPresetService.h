#pragma once

// ------------------------------------------------------------------
// AgentPresetService.h
// ------------------------------------------------------------------
// Agent 预设的“功能逻辑”：
//   - 通过 agentPresets/list 拉取预设；
//   - 解析成结构化列表（id / 名称 / 是否服务端默认）；
//   - 从列表里算出“当前该显示哪一个”；
//   - 把某个预设写进服务端设置文档，作为“此后新建会话”的默认。
//
// “默认预设”是**服务端的一个设置字段**，不是预设自身的属性、也不是客户端的记忆：
//   * 读：agentPresets/list 每行带的 isDefault（服务端按 settings 里那个字段算的）；
//   * 写：settings/update(ns = "agent-presets", patch = { default: <id> })，
//     落盘到 <DSH_HOME>/settings.yaml。
// 这与原版 web 客户端完全一致（见 dsh-client-ui-agent-preset 的 writeDefaultPreset），
// 所以客户端这边**不保存任何一份本地副本**。
//
// 生效范围由服务端定：默认值只在**新建会话**时被解析成初值；
// 已有会话各自的预设记录在会话日志里（头部 + agent-preset/selected 事件），
// 服务端不会回头改写它们。也就是说改默认“只影响新会话”。
//
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
	// 承载“默认预设”的服务端设置命名空间（dsh-agent-presets 注册的那个）。
	inline constexpr const char* kSettingsNamespace = "agent-presets";

	// 写入默认预设用的 patch：只并这一个字段，不碰该命名空间的其它内容。
	inline QJsonObject defaultPatch(const QString& presetId)
	{
		QJsonObject patch;
		patch.insert(QStringLiteral("default"), presetId);
		return patch;
	}

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
	//
	// 注：本项目的本地记忆已随“默认预设归服务端”一并去掉（调用方传空串），
	// 所以实际生效的顺序就是“服务端 isDefault → 第一项”。保留这个参数是为了
	// 让本函数仍可复用于“暂存选择”那类场景（原版新建会话 chip 就是暂存的，
	// 用完即清、刻意不写默认值）。
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

	// 把某个预设设为服务端默认（原版 web 的“设为默认”就是这一次写入）。
	//
	// 只影响此后**新建**的会话：已有会话的预设记录在各自会话日志里，服务端不回头改写。
	// 服务端对该 namespace 只有 schema 约束（{ default: string }），**不校验 id 是否存在**，
	// 所以调用方必须保证 presetId 来自 agentPresets/list 的名单 —— 否则之后每一次
	// 新建会话都会以 agent-preset/not-found 失败。本函数只挡空串。
	void persistDefault(
		DshApiClient* api,
		const QString& presetId,
		const std::function<void(const QString& presetId)>& onSaved,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);
}
