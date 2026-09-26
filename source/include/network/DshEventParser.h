#pragma once

// DSH mux 流 JSON 的解析接口，独立于 UI。

#include <QJsonObject>
#include <QString>

struct ToolCallInfo
{
	QString name;
	QJsonObject arguments;
	bool valid = false;
};

struct ToolResultInfo
{
	QString message;
	bool valid = false;
};

// 各类事件 → 可读字段的提取：assistant/chunk → chunk 类型，assistant/message → 思考块 / 直接回复，
// tool/call → 调用信息，tool/result → 执行结果；文本提取兼容 AI 回复 / 用户消息 / 流式片段。
QString extractEventText(const QJsonObject& event);
QString extractChunkType(const QJsonObject& event);
QString extractThinking(const QJsonObject& event);
QString extractReply(const QJsonObject& event);
ToolCallInfo extractToolCall(const QJsonObject& event);
ToolResultInfo extractToolResult(const QJsonObject& event);
