// ------------------------------------------------------------------
// DshEventParser.cpp
// ------------------------------------------------------------------
// DSH Mux 流 JSON 解析接口实现。
// 这些函数不依赖任何 UI 类，可以单独复用。
// ------------------------------------------------------------------

#include "DshEventParser.h"

#include <QJsonArray>
#include <QJsonDocument>

QString extractEventText(const QJsonObject& event)
{
	const QString type = event.value(QStringLiteral("type")).toString();
	const QJsonObject data = event.value(QStringLiteral("data")).toObject();

	// 完整消息：assistant/message、user/message
	if (type == QStringLiteral("assistant/message")
		|| type == QStringLiteral("user/message")) {
		QJsonObject message = data.value(QStringLiteral("message")).toObject();
		// 兼容历史记录里 user/message 直接把消息放在 data 下的情况
		if (message.isEmpty())
			message = data;
		const QJsonValue content = message.value(QStringLiteral("content"));

		if (content.isString())
			return content.toString();

		if (content.isArray()) {
			QStringList parts;
			const QJsonArray blocks = content.toArray();
			for (const auto& blockValue : blocks) {
				const QJsonObject block = blockValue.toObject();
				if (block.value(QStringLiteral("type")).toString() == QStringLiteral("text")
					|| block.contains(QStringLiteral("text"))) {
					parts << block.value(QStringLiteral("text")).toString();
				}
			}
			if (!parts.isEmpty())
				return parts.join(QLatin1Char('\n'));
		}
	}

	// 流式增量：assistant/chunk
	if (type == QStringLiteral("assistant/chunk")) {
		const QJsonValue chunk = data.value(QStringLiteral("chunk"));
		if (chunk.isString())
			return chunk.toString();
		if (chunk.isObject()) {
			const QJsonObject chunkObj = chunk.toObject();
			const QJsonValue text = chunkObj.value(QStringLiteral("text"));
			if (text.isString())
				return text.toString();
		}
	}

	// 兜底：某些事件直接带 text
	const QJsonValue textValue = data.value(QStringLiteral("text"));
	if (textValue.isString())
		return textValue.toString();

	return QString();
}

QString extractChunkType(const QJsonObject& event)
{
	const QJsonObject data = event.value(QStringLiteral("data")).toObject();
	const QJsonValue chunk = data.value(QStringLiteral("chunk"));

	if (chunk.isObject())
		return chunk.toObject().value(QStringLiteral("type")).toString();

	return QString();
}

QString extractThinking(const QJsonObject& event)
{
	const QJsonObject data = event.value(QStringLiteral("data")).toObject();
	QJsonObject message = data.value(QStringLiteral("message")).toObject();
	if (message.isEmpty())
		message = data;
	const QJsonValue content = message.value(QStringLiteral("content"));

	if (!content.isArray())
		return QString();

	QStringList parts;
	const QJsonArray blocks = content.toArray();
	for (const auto& blockValue : blocks) {
		const QJsonObject block = blockValue.toObject();
		if (block.value(QStringLiteral("type")).toString() == QStringLiteral("reasoning")) {
			const QString text = block.value(QStringLiteral("text")).toString();
			if (!text.isEmpty())
				parts << text;
		}
	}

	return parts.join(QLatin1Char('\n'));
}

QString extractReply(const QJsonObject& event)
{
	const QJsonObject data = event.value(QStringLiteral("data")).toObject();
	QJsonObject message = data.value(QStringLiteral("message")).toObject();
	if (message.isEmpty())
		message = data;
	const QJsonValue content = message.value(QStringLiteral("content"));

	if (content.isString())
		return content.toString();

	if (!content.isArray())
		return QString();

	QStringList parts;
	const QJsonArray blocks = content.toArray();
	for (const auto& blockValue : blocks) {
		const QJsonObject block = blockValue.toObject();
		if (block.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
			const QString text = block.value(QStringLiteral("text")).toString();
			if (!text.isEmpty())
				parts << text;
		}
	}

	return parts.join(QLatin1Char('\n'));
}

ToolCallInfo extractToolCall(const QJsonObject& event)
{
	ToolCallInfo info;
	const QJsonObject data = event.value(QStringLiteral("data")).toObject();
	info.name = data.value(QStringLiteral("name")).toString();
	info.valid = !info.name.isEmpty();

	const QJsonValue argumentsValue = data.value(QStringLiteral("arguments"));
	if (argumentsValue.isObject()) {
		info.arguments = argumentsValue.toObject();
	}
	else if (argumentsValue.isString()) {
		info.arguments = QJsonDocument::fromJson(
			argumentsValue.toString().toUtf8()).object();
	}

	return info;
}

ToolResultInfo extractToolResult(const QJsonObject& event)
{
	ToolResultInfo info;
	const QJsonObject data = event.value(QStringLiteral("data")).toObject();
	info.message = data.value(QStringLiteral("message")).toString();
	info.error = data.value(QStringLiteral("error")).toString();
	info.valid = !info.message.isEmpty() || !info.error.isEmpty();
	return info;
}

ApprovalInfo extractApproval(const QJsonObject& payload)
{
	ApprovalInfo info;
	info.sessionId = payload.value(QStringLiteral("sessionId")).toString();
	info.approvalId = payload.value(QStringLiteral("approvalId")).toString();
	info.toolName = payload.value(QStringLiteral("toolName")).toString();
	info.reason = payload.value(QStringLiteral("reason")).toString();
	info.valid = !info.sessionId.isEmpty() && !info.approvalId.isEmpty();
	return info;
}

QList<QuestionInfo> extractQuestions(const QJsonObject& payload)
{
	QList<QuestionInfo> result;
	const QJsonArray questions = payload.value(QStringLiteral("questions")).toArray();

	for (const auto& questionValue : questions) {
		const QJsonObject question = questionValue.toObject();
		QuestionInfo info;
		info.id = question.value(QStringLiteral("id")).toString();
		info.question = question.value(QStringLiteral("question")).toString();
		info.detail = question.value(QStringLiteral("detail")).toString();
		info.multiSelect = question.value(QStringLiteral("multiSelect")).toBool();

		const QJsonArray options = question.value(QStringLiteral("options")).toArray();
		for (const auto& optionValue : options) {
			const QJsonObject option = optionValue.toObject();
			info.options << option.value(QStringLiteral("label")).toString();
		}

		result.append(info);
	}

	return result;
}