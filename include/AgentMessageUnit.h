#pragma once

// ------------------------------------------------------------------
// RichTextView.h
// ------------------------------------------------------------------
// 可复用的富文本展示控件。
//
// 功能：
//   - 基于 QTextBrowser，只读、可选择、可复制
//   - 支持 Markdown / HTML 追加显示
//   - 支持根据内容自动调整高度
// ------------------------------------------------------------------

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTextBrowser>

// 流式消息片段，按出现顺序保存，保证思考/工具调用/回复交错显示
struct StreamSegment
{
    enum Type { Thinking, Reply, ToolCall, ToolResult };
    Type type = Reply;
    QString content;
    QString toolName; // 仅 ToolCall 使用
};

/**
 * 可复用的富文本展示控件。
 *
 * 使用示例：
 * @code
 * RichTextView *view = new RichTextView;
 * view->setFixedWidth(800);
 * view->appendMarkdownWithCodeShadow(reply);
 * view->appendHtml("<p>普通 HTML</p>");
 * @endcode
 */
class AgentMessageUnit : public QTextBrowser
{
    Q_OBJECT

public:
    /** 默认文本框宽度。 */
    static constexpr int DefaultWidth = 720;

    explicit AgentMessageUnit(QWidget *parent = nullptr);
    ~AgentMessageUnit() override;

    /** 追加一段带代码块阴影的 Markdown 渲染内容。 */
    void appendMarkdownWithCodeShadow(const QString &markdown);

    /** 在已有内容后面插入一个段落分隔，用于合并多段输出时明确换行。 */
    void appendSeparator();

    /** 追加一段可折叠思考内容。 */
    void appendThinking(const QString &thinking);

    /** 追加一个可折叠的工具调用块。 */
    void appendToolCall(const QString &name, const QString &argumentsHtml);

    /** 追加一个可折叠的工具结果块。 */
    void appendToolResult(const QString &resultHtml);

    /** 追加一段 HTML 内容。 */
    void appendHtml(const QString &html);

    /** 清空文档以及内部已记录的 segment/思考块，准备重新渲染整段内容。 */
    void resetContent();

    /** 追加一个流式片段。 */
    void appendStreamChunk(StreamSegment::Type type, const QString &content,
                           const QString &toolName = QString());

    /** 把累积的流式片段渲染到当前气泡。 */
    void flushStream();

    /** 清空流式片段缓存。 */
    void clearStreamSegments();

    /** 根据当前文档内容重新计算并设置控件高度。 */
    void updateHeightToContent();

private:
    struct ThinkingBlock
    {
        QString content;
        bool expanded = false;
    };

    struct ToolBlock
    {
        QString title;
        QString content;
        bool expanded = false;
    };

    struct Segment
    {
        enum Type
        {
            Thinking,
            Tool,
            Markdown,
            Html,
            StreamText
        };
        Type type = Markdown;
        int thinkingIndex = -1;
        int toolIndex = -1;
        QString text;
    };

    QList<ThinkingBlock> m_thinkingBlocks;
    QList<ToolBlock> m_toolBlocks;
    QList<Segment> m_segments;
    QList<StreamSegment> m_streamSegments;
    QSet<int> m_expandedThinkingIndices; // 流式重建时保留用户展开状态
    QSet<int> m_expandedToolIndices; // 流式重建时保留工具块展开状态

    bool m_rebuilding = false;

    void rebuild();
    void insertMarkdownWithCodeShadow(const QString &markdown);
    void insertHtml(const QString &html);
    void insertThinking(int index);
    void toggleThinking(int index);
    void insertTool(int index);
    void toggleTool(int index);
    QString thinkingPreview(const QString &content) const;

    /** 生成带阴影的代码块 HTML（最初版样式）。 */
    QString buildCodeShadowHtml(const QString &language, const QString &code);

    /** 把 Markdown 行内代码替换成占位符，避免 Qt 解析丢失样式。 */
    QString replaceInlineCodeWithPlaceholders(const QString &markdown, QStringList &codes) const;

    /** 把占位符恢复成带样式的行内代码 HTML。 */
    QString restoreInlineCodeHtml(const QString &html, const QStringList &codes) const;

};
