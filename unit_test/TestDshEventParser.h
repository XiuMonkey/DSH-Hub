#pragma once

// ------------------------------------------------------------------
// TestDshEventParser.h
// ------------------------------------------------------------------
// DshEventParser 纯解析函数的单元测试。
// ------------------------------------------------------------------

#include <QObject>

class TestDshEventParser : public QObject
{
    Q_OBJECT

private slots:
    void extractEventText_assistantMessageString();
    void extractEventText_userMessageLegacy();
    void extractEventText_contentArray();
    void extractEventText_chunkString();
    void extractEventText_chunkObject();
    void extractEventText_fallbackText();
    void extractEventText_unknownTypeEmpty();

    void extractChunkType_object();
    void extractChunkType_missing();

    void extractThinking_onlyReasoningBlocks();
    void extractThinking_ignoresTextBlocks();
    void extractThinking_stringContentReturnsEmpty();

    void extractReply_stringContent();
    void extractReply_textBlocksOnly();
    void extractReply_ignoresReasoning();

    void extractToolCall_objectArguments();
    void extractToolCall_jsonStringArguments();
    void extractToolCall_invalid();

    void extractToolResult_valid();
    void extractToolResult_invalid();

    void extractApproval_valid();
    void extractApproval_missingApprovalId();

    void extractQuestions_returnsOptions();
    void extractQuestions_empty();
};