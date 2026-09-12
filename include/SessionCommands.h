#pragma once

// ------------------------------------------------------------------
// SessionCommands.h
// ------------------------------------------------------------------
// DSH RPC 请求载荷与常见会话操作的统一构造入口。
// 把散落在 DSHHub 各回调里的 JSON payload 拼装收敛到这里，
// 控制器只负责编排（何时调用、成功后怎么改 UI），不再手搓载荷。
// 纯 header + inline，无额外链接依赖。
// ------------------------------------------------------------------

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace SessionCommands
{
	// workspace.create
	inline QJsonObject workspaceCreate(const QString& path)
	{
		QJsonObject payload;
		payload.insert(QStringLiteral("path"), path);
		return payload;
	}

	// session.create：可带工作区与默认 agent preset
	inline QJsonObject sessionCreate(const QString& workspaceId = QString(),
		const QString& agentPreset = QString())
	{
		QJsonObject payload;
		if (!workspaceId.isEmpty())
			payload.insert(QStringLiteral("workspaceId"), workspaceId);
		if (!agentPreset.isEmpty())
			payload.insert(QStringLiteral("agentPreset"), agentPreset);
		return payload;
	}

	// session.prompt：queue 模式 + 一条 text 内容
	inline QJsonObject sessionPrompt(const QString& sessionId, const QString& text)
	{
		QJsonObject payload;
		payload.insert(QStringLiteral("sessionId"), sessionId);
		payload.insert(QStringLiteral("mode"), QStringLiteral("queue"));

		QJsonArray content;
		QJsonObject textPart;
		textPart.insert(QStringLiteral("type"), QStringLiteral("text"));
		textPart.insert(QStringLiteral("text"), text);
		content.append(textPart);
		payload.insert(QStringLiteral("content"), content);
		return payload;
	}

	// session.cancel
	inline QJsonObject sessionCancel(const QString& sessionId)
	{
		QJsonObject payload;
		payload.insert(QStringLiteral("sessionId"), sessionId);
		return payload;
	}

	// workspace.archiveSession（删除会话）
	inline QJsonObject sessionArchive(const QString& sessionId)
	{
		QJsonObject payload;
		payload.insert(QStringLiteral("sessionId"), sessionId);
		return payload;
	}

	// agentPreset.select
	inline QJsonObject agentPresetSelect(const QString& sessionId, const QString& presetId)
	{
		QJsonObject payload;
		payload.insert(QStringLiteral("sessionId"), sessionId);
		payload.insert(QStringLiteral("agentPreset"), presetId);
		return payload;
	}
}
