#pragma once

// ------------------------------------------------------------------
// SessionCommands.h
// ------------------------------------------------------------------
// DSH RPC 请求载荷与常见会话操作的统一构造入口。
// 把散落在 DSHHub 各回调里的 JSON payload 拼装收敛到这里，
// 控制器只负责编排（何时调用、成功后怎么改 UI），不再手搓载荷。
// 纯 header + inline，无额外链接依赖。
//
// dsh 0.1.5 约定（重要）：
//   * endpoint 是 `<namespace>/<method>`（斜杠），不再用点号；
//   * 请求体 payload 恰好只有一个 `args` 对象，DshApiClient 负责包这一层；
//   * 因此本文件里的构造函数返回的都是 **args 的内容**，键名必须是描述符里的
//     wire 名（多数方法是 `request`，session/list 是 `_request`，
//     无参方法是空对象，agentPresets/select 是 `agentId` + `agentPreset`）。
// ------------------------------------------------------------------

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QUuid>

namespace SessionCommands
{
	// 无参端点的 args：session/modelCatalog、agentPresets/list、
	// llm/listProviders、llm/listConfigurableProviders、settings/describe 等
	inline QJsonObject emptyArgs()
	{
		return QJsonObject();
	}

	// session/list 的 args：wire 名是 _request（描述符里参数名就叫 _request）
	inline QJsonObject sessionList()
	{
		QJsonObject args;
		args.insert(QStringLiteral("_request"), QJsonObject());
		return args;
	}

	// workspace/create -> args { request: { path } }
	inline QJsonObject workspaceCreate(const QString& path)
	{
		QJsonObject request;
		request.insert(QStringLiteral("path"), path);

		QJsonObject args;
		args.insert(QStringLiteral("request"), request);
		return args;
	}

	// session/create -> args { request: { workspaceId?, cwd?, sessionId?, agentPreset? } }
	inline QJsonObject sessionCreate(const QString& workspaceId = QString(),
		const QString& agentPreset = QString())
	{
		QJsonObject request;
		if (!workspaceId.isEmpty())
			request.insert(QStringLiteral("workspaceId"), workspaceId);
		if (!agentPreset.isEmpty())
			request.insert(QStringLiteral("agentPreset"), agentPreset);

		QJsonObject args;
		args.insert(QStringLiteral("request"), request);
		return args;
	}

	// session/page -> args { request: { address, throughSeq, beforeSeq?, maxMessages? } }
	//
	// 这是 0.1.5 唯一的"读历史"入口，两个用途共用本函数：
	//   * 首屏上翻（"加载更多"）：beforeSeq = 当前内容里最早一条的 seq
	//   * 预取/首屏回落：不传 beforeSeq，语义是"throughSeq 往前最近一页"
	// throughSeq 必填（服务端缺它会回 gateway/input-invalid），来源二选一：
	//   follow 快照的 cursor，或 session/list 行的 projections.asOfSeq（两者相等）。
	// maxMessages 数的是**消息**而不是事件（一条消息通常含十几条事件记录）。
	inline QJsonObject sessionPage(const QString& sessionId, int throughSeq,
		int maxMessages = 0, int beforeSeq = 0)
	{
		QJsonObject address;
		address.insert(QStringLiteral("kind"), QStringLiteral("session"));
		address.insert(QStringLiteral("sessionId"), sessionId);

		QJsonObject request;
		request.insert(QStringLiteral("address"), address);
		request.insert(QStringLiteral("throughSeq"), throughSeq);
		if (maxMessages > 0)
			request.insert(QStringLiteral("maxMessages"), maxMessages);
		if (beforeSeq > 0)
			request.insert(QStringLiteral("beforeSeq"), beforeSeq);

		QJsonObject args;
		args.insert(QStringLiteral("request"), request);
		return args;
	}

	// session/page 与 session/follow 的记录都是 {type:"event", event:{…}}；
	// 渲染路径只认裸事件对象，所以统一用这里取出内层事件。
	inline QJsonArray eventsFromRecords(const QJsonArray& records)
	{
		QJsonArray events;
		for (const auto& record : records) {
			const QJsonObject event = record.toObject().value(QStringLiteral("event")).toObject();
			if (!event.isEmpty())
				events.append(event);
		}
		return events;
	}

	// session/prompt -> args { request: { requestId, sessionId, mode, content } }
	// 0.1.5 起 requestId 是必填：客户端铸一个 id，服务端把它落到那条用户消息上。
	inline QJsonObject sessionPrompt(const QString& sessionId, const QString& text)
	{
		QJsonArray content;
		QJsonObject textPart;
		textPart.insert(QStringLiteral("type"), QStringLiteral("text"));
		textPart.insert(QStringLiteral("text"), text);
		content.append(textPart);

		QJsonObject request;
		request.insert(QStringLiteral("requestId"),
			QUuid::createUuid().toString(QUuid::WithoutBraces));
		request.insert(QStringLiteral("sessionId"), sessionId);
		request.insert(QStringLiteral("mode"), QStringLiteral("queue"));
		request.insert(QStringLiteral("content"), content);

		QJsonObject args;
		args.insert(QStringLiteral("request"), request);
		return args;
	}

	// session/cancel -> args { request: { sessionId } }
	inline QJsonObject sessionCancel(const QString& sessionId)
	{
		QJsonObject request;
		request.insert(QStringLiteral("sessionId"), sessionId);

		QJsonObject args;
		args.insert(QStringLiteral("request"), request);
		return args;
	}

	// workspace/archiveSession（删除会话）-> args { request: { sessionId } }
	inline QJsonObject sessionArchive(const QString& sessionId)
	{
		QJsonObject request;
		request.insert(QStringLiteral("sessionId"), sessionId);

		QJsonObject args;
		args.insert(QStringLiteral("request"), request);
		return args;
	}

	// agentPresets/select -> args { agentId, agentPreset }
	// 注意：这不是 `request` 包装，而是两个平铺的 wire 名；agentId 就是 sessionId
	// （服务端用 agent lookup 把 SessionId 解析成活的 Agent）。
	//
	// 语义是"改这个会话当前跑的预设"，且**只在空白期可用**（会话跑过任一轮就
	// 永久锁定，服务端回 agent-preset/locked）。原版 web 用它承载"新建会话 chip
	// 的暂存选择"（用完即清、刻意不写默认值）。
	//
	// Hub 目前没有调用点：设置页改的是**默认值**（走 settings/update），
	// 不该顺手改当前会话。这里保留是作为协议收录，将来要做"临时换模式"时用得上。
	inline QJsonObject agentPresetSelect(const QString& sessionId, const QString& presetId)
	{
		QJsonObject args;
		args.insert(QStringLiteral("agentId"), sessionId);
		args.insert(QStringLiteral("agentPreset"), presetId);
		return args;
	}

	// session/selectModel -> args { request: { sessionId, provider, model, reasoningEffort? } }
	inline QJsonObject sessionSelectModel(const QString& sessionId,
		const QString& provider,
		const QString& model,
		const QString& reasoningEffort = QString())
	{
		QJsonObject request;
		request.insert(QStringLiteral("sessionId"), sessionId);
		request.insert(QStringLiteral("provider"), provider);
		request.insert(QStringLiteral("model"), model);
		// 省略即提供方默认档位；显式档位才写入
		if (!reasoningEffort.isEmpty())
			request.insert(QStringLiteral("reasoningEffort"), reasoningEffort);

		QJsonObject args;
		args.insert(QStringLiteral("request"), request);
		return args;
	}

	// settings/mutate -> args { ns, ops, expectedRevision? }
	inline QJsonObject settingsMutate(const QString& ns, const QJsonArray& ops)
	{
		QJsonObject args;
		args.insert(QStringLiteral("ns"), ns);
		args.insert(QStringLiteral("ops"), ops);
		return args;
	}

	// settings/update -> args { ns, patch, expectedRevision? }
	//
	// 与 mutate 的区别：这条是"把 patch 并进该命名空间的 user 段"（浅合并），
	// 不是按路径寻址的增删改。原版 web 客户端写默认预设用的就是这条：
	//   settings.update("agent-presets", { default: id }, undefined)
	//
	// expectedRevision 描述符里标了 acceptsUndefined，整条省掉即"无条件写"
	// （不做乐观并发校验）—— 与原版传 undefined 等价。
	inline QJsonObject settingsUpdate(const QString& ns, const QJsonObject& patch)
	{
		QJsonObject args;
		args.insert(QStringLiteral("ns"), ns);
		args.insert(QStringLiteral("patch"), patch);
		return args;
	}

	// credentials/describe -> args { refs: [ref, …] }
	inline QJsonObject credentialsDescribe(const QJsonArray& refs)
	{
		QJsonObject args;
		args.insert(QStringLiteral("refs"), refs);
		return args;
	}

	// credentials/set -> args { ref, value }
	inline QJsonObject credentialsSet(const QString& ref, const QString& value)
	{
		QJsonObject args;
		args.insert(QStringLiteral("ref"), ref);
		args.insert(QStringLiteral("value"), value);
		return args;
	}
}
