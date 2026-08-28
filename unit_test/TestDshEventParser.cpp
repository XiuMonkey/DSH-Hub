#include "TestDshEventParser.h"

#include "DshEventParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTest>

namespace
{

QJsonObject objectFromJson(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

} // namespace

void TestDshEventParser::extractEventText_assistantMessageString()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "assistant/message",
        "data": {
            "message": {
                "content": "Hello, DSH!"
            }
        }
    })json");

    QCOMPARE(extractEventText(event), QStringLiteral("Hello, DSH!"));
}

void TestDshEventParser::extractEventText_userMessageLegacy()
{
    // Legacy user/message compatibility: user/message may put message directly in data.
    const QJsonObject event = objectFromJson(R"json({
        "type": "user/message",
        "data": {
            "content": "legacy user message"
        }
    })json");

    QCOMPARE(extractEventText(event), QStringLiteral("legacy user message"));
}

void TestDshEventParser::extractEventText_contentArray()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "assistant/message",
        "data": {
            "message": {
                "content": [
                    { "type": "text", "text": "first" },
                    { "type": "reasoning", "text": "hidden" },
                    { "text": "third" }
                ]
            }
        }
    })json");

    QCOMPARE(extractEventText(event), QStringLiteral("first\nhidden\nthird"));
}

void TestDshEventParser::extractEventText_chunkString()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "assistant/chunk",
        "data": {
            "chunk": "stream"
        }
    })json");

    QCOMPARE(extractEventText(event), QStringLiteral("stream"));
}

void TestDshEventParser::extractEventText_chunkObject()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "assistant/chunk",
        "data": {
            "chunk": { "type": "text", "text": "chunk object" }
        }
    })json");

    QCOMPARE(extractEventText(event), QStringLiteral("chunk object"));
}

void TestDshEventParser::extractEventText_fallbackText()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "some/event",
        "data": {
            "text": "fallback"
        }
    })json");

    QCOMPARE(extractEventText(event), QStringLiteral("fallback"));
}

void TestDshEventParser::extractEventText_unknownTypeEmpty()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "unknown/event",
        "data": {}
    })json");

    QVERIFY(extractEventText(event).isEmpty());
}

void TestDshEventParser::extractChunkType_object()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "assistant/chunk",
        "data": {
            "chunk": { "type": "reasoning", "text": "thinking" }
        }
    })json");

    QCOMPARE(extractChunkType(event), QStringLiteral("reasoning"));
}

void TestDshEventParser::extractChunkType_missing()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "assistant/chunk",
        "data": { "chunk": "text" }
    })json");

    QVERIFY(extractChunkType(event).isEmpty());
}

void TestDshEventParser::extractThinking_onlyReasoningBlocks()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "assistant/message",
        "data": {
            "message": {
                "content": [
                    { "type": "reasoning", "text": "step 1" },
                    { "type": "reasoning", "text": "step 2" },
                    { "type": "text", "text": "final answer" }
                ]
            }
        }
    })json");

    QCOMPARE(extractThinking(event), QStringLiteral("step 1\nstep 2"));
}

void TestDshEventParser::extractThinking_ignoresTextBlocks()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "assistant/message",
        "data": {
            "message": {
                "content": [
                    { "type": "text", "text": "answer" }
                ]
            }
        }
    })json");

    QVERIFY(extractThinking(event).isEmpty());
}

void TestDshEventParser::extractThinking_stringContentReturnsEmpty()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "assistant/message",
        "data": {
            "message": {
                "content": "plain string"
            }
        }
    })json");

    QVERIFY(extractThinking(event).isEmpty());
}

void TestDshEventParser::extractReply_stringContent()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "assistant/message",
        "data": {
            "message": {
                "content": "direct reply"
            }
        }
    })json");

    QCOMPARE(extractReply(event), QStringLiteral("direct reply"));
}

void TestDshEventParser::extractReply_textBlocksOnly()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "assistant/message",
        "data": {
            "message": {
                "content": [
                    { "type": "text", "text": "part 1" },
                    { "type": "reasoning", "text": "hidden" },
                    { "type": "text", "text": "part 2" }
                ]
            }
        }
    })json");

    QCOMPARE(extractReply(event), QStringLiteral("part 1\npart 2"));
}

void TestDshEventParser::extractReply_ignoresReasoning()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "assistant/message",
        "data": {
            "message": {
                "content": [
                    { "type": "reasoning", "text": "thinking" }
                ]
            }
        }
    })json");

    QVERIFY(extractReply(event).isEmpty());
}

void TestDshEventParser::extractToolCall_objectArguments()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "tool/call",
        "data": {
            "name": "read_file",
            "arguments": {
                "path": "C:/tmp/a.txt",
                "line": 10
            }
        }
    })json");

    const ToolCallInfo info = extractToolCall(event);
    QVERIFY(info.valid);
    QCOMPARE(info.name, QStringLiteral("read_file"));
    QCOMPARE(info.arguments.value(QStringLiteral("path")).toString(), QStringLiteral("C:/tmp/a.txt"));
    QCOMPARE(info.arguments.value(QStringLiteral("line")).toInt(), 10);
}

void TestDshEventParser::extractToolCall_jsonStringArguments()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "tool/call",
        "data": {
            "name": "run_command",
            "arguments": "{ \"command\": \"dir\" }"
        }
    })json");

    const ToolCallInfo info = extractToolCall(event);
    QVERIFY(info.valid);
    QCOMPARE(info.name, QStringLiteral("run_command"));
    QCOMPARE(info.arguments.value(QStringLiteral("command")).toString(), QStringLiteral("dir"));
}

void TestDshEventParser::extractToolCall_invalid()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "tool/call",
        "data": { "arguments": {} }
    })json");

    const ToolCallInfo info = extractToolCall(event);
    QVERIFY(!info.valid);
    QVERIFY(info.name.isEmpty());
}

void TestDshEventParser::extractToolResult_valid()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "tool/result",
        "data": {
            "message": "done",
            "error": ""
        }
    })json");

    const ToolResultInfo info = extractToolResult(event);
    QVERIFY(info.valid);
    QCOMPARE(info.message, QStringLiteral("done"));
    QVERIFY(info.error.isEmpty());
}

void TestDshEventParser::extractToolResult_invalid()
{
    const QJsonObject event = objectFromJson(R"json({
        "type": "tool/result",
        "data": {}
    })json");

    const ToolResultInfo info = extractToolResult(event);
    QVERIFY(!info.valid);
    QVERIFY(info.message.isEmpty());
    QVERIFY(info.error.isEmpty());
}

void TestDshEventParser::extractApproval_valid()
{
    const QJsonObject payload = objectFromJson(R"json({
        "sessionId": "sess-1",
        "approvalId": "appr-1",
        "toolName": "run_command",
        "reason": "needs permission"
    })json");

    const ApprovalInfo info = extractApproval(payload);
    QVERIFY(info.valid);
    QCOMPARE(info.sessionId, QStringLiteral("sess-1"));
    QCOMPARE(info.approvalId, QStringLiteral("appr-1"));
    QCOMPARE(info.toolName, QStringLiteral("run_command"));
    QCOMPARE(info.reason, QStringLiteral("needs permission"));
}

void TestDshEventParser::extractApproval_missingApprovalId()
{
    const QJsonObject payload = objectFromJson(R"json({
        "sessionId": "sess-1",
        "toolName": "run_command"
    })json");

    const ApprovalInfo info = extractApproval(payload);
    QVERIFY(!info.valid);
    QCOMPARE(info.sessionId, QStringLiteral("sess-1"));
    QVERIFY(info.approvalId.isEmpty());
}

void TestDshEventParser::extractQuestions_returnsOptions()
{
    const QJsonObject payload = objectFromJson(R"json({
        "questions": [
            {
                "id": "q1",
                "question": "Choose an option",
                "detail": "detail text",
                "multiSelect": true,
                "options": [
                    { "label": "A" },
                    { "label": "B" }
                ]
            }
        ]
    })json");

    const QList<QuestionInfo> questions = extractQuestions(payload);
    QCOMPARE(questions.size(), 1);
    const QuestionInfo &info = questions.first();
    QCOMPARE(info.id, QStringLiteral("q1"));
    QCOMPARE(info.question, QStringLiteral("Choose an option"));
    QCOMPARE(info.detail, QStringLiteral("detail text"));
    QVERIFY(info.multiSelect);
    QCOMPARE(info.options, QStringList() << QStringLiteral("A") << QStringLiteral("B"));
}

void TestDshEventParser::extractQuestions_empty()
{
    const QJsonObject payload = objectFromJson(R"json({
        "questions": []
    })json");

    QVERIFY(extractQuestions(payload).isEmpty());
}
