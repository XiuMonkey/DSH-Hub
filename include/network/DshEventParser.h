#pragma once

// DSH Mux 流 JSON 解析接口，独立于 UI。

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

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
	QString error;
	bool valid = false;
};

// 审批请求信息。
struct ApprovalInfo
{
	QString sessionId;
	QString approvalId;
	QString toolName;
	QString reason;
	bool valid = false;
};

// 单个提问信息。
struct QuestionInfo
{
	QString id;
	QString question;
	QString detail;
	QStringList options;
	bool multiSelect = false;
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

// 从 approval/requested 帧提取审批信息。
ApprovalInfo extractApproval(const QJsonObject& payload);

// 从 question/requested 帧提取问题列表。
QList<QuestionInfo> extractQuestions(const QJsonObject& payload);
