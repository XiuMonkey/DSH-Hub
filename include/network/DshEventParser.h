#pragma once

// DSH Mux 流 JSON 解析接口，独立于 UI。

#include <QJsonObject>
#include <QString>

// 工具调用信息。
struct ToolCallInfo
{
	QString name;
	QJsonObject arguments;
	bool valid = false;
};

// 工具执行结果信息。
struct ToolResultInfo
{
	QString message;
	bool valid = false;
};

// 从 SessionEvent 提取可读文本（AI 回复 / 用户消息 / 流式片段）。
QString extractEventText(const QJsonObject& event);

// 从 assistant/chunk 提取 chunk 类型（"text" / "reasoning"）。
QString extractChunkType(const QJsonObject& event);

// 从 assistant/message 提取思考内容（reasoning block）。
QString extractThinking(const QJsonObject& event);

// 从 assistant/message 提取直接回复（text block）。
QString extractReply(const QJsonObject& event);

// 从 tool/call 提取工具调用信息。
ToolCallInfo extractToolCall(const QJsonObject& event);

// 从 tool/result 提取工具执行结果信息。
ToolResultInfo extractToolResult(const QJsonObject& event);

