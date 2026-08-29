#pragma once

// ------------------------------------------------------------------
// DshEventParser.h
// ------------------------------------------------------------------
// DSH Mux 流 JSON 解析接口，独立于 UI。
//
// 这些函数接收 DSH 推送的 JSON 对象，返回结构化的解析结果，
// 方便上层界面或其它模块直接使用。
// ------------------------------------------------------------------

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

// 从 SessionEvent 中提取可读文本（如 AI 回复、用户消息、流式片段）。
QString extractEventText(const QJsonObject& event);

// 从 assistant/chunk 中提取 chunk 类型（例如 "text" / "reasoning"）。
QString extractChunkType(const QJsonObject& event);

// 从 assistant/message 中提取思考内容（reasoning block）。
QString extractThinking(const QJsonObject& event);

// 从 assistant/message 中提取直接回复（text block）。
QString extractReply(const QJsonObject& event);

// 从 tool/call 事件中提取工具调用信息。
ToolCallInfo extractToolCall(const QJsonObject& event);

// 从 tool/result 事件中提取工具执行结果信息。
ToolResultInfo extractToolResult(const QJsonObject& event);

// 从 approval/requested 帧中提取审批信息。
ApprovalInfo extractApproval(const QJsonObject& payload);

// 从 question/requested 帧中提取问题列表。
QList<QuestionInfo> extractQuestions(const QJsonObject& payload);
