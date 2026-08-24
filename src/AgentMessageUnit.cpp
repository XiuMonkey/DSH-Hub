// ------------------------------------------------------------------
// RichTextView.cpp
// ------------------------------------------------------------------
// 可复用富文本展示控件的实现。
// ------------------------------------------------------------------

#include "AgentMessageUnit.h"
#include "CodeHighlighter.h"

#include <QAbstractTextDocumentLayout>
#include <QFrame>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QRegularExpression>
#include <QTextCursor>
#include <QTextDocument>

AgentMessageUnit::AgentMessageUnit(QWidget *parent)
    : QTextBrowser(parent)
{
    setOpenExternalLinks(true);

    // 浅灰色圆角背景，去掉默认边框
    setStyleSheet(QStringLiteral(
        "AgentMessageUnit {"
        "  background-color: #FFFFFF;"
        "  border-radius: 12px;"
        "}"
    ));
    setViewportMargins(8, 8, 8, 8);
    viewport()->setAutoFillBackground(false);
    setFrameShape(QFrame::NoFrame);
    setFrameShadow(QFrame::Plain);

    // 关闭内部滚动，高度由 updateHeightToContent() 控制
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    setFixedWidth(DefaultWidth);
    setFixedHeight(600); // 初始高度，内容变化后会重新计算
    document()->setDocumentMargin(0);

    // 内容变化后重新计算高度
    connect(document(), &QTextDocument::contentsChanged,
            this, &AgentMessageUnit::updateHeightToContent);

    // 点击思考标题时切换展开/收起
    setOpenLinks(false);
    connect(this, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        if (url.scheme() == QStringLiteral("dsh")
            && url.host() == QStringLiteral("thinking")) {
            bool ok = false;
            const int index = url.path().remove(0, 1).toInt(&ok);
            if (ok)
                toggleThinking(index);
        }
    });
}

AgentMessageUnit::~AgentMessageUnit()
{
    // 析构期间断开文档信号，避免基类销毁文档时再次调用本类方法
    disconnect(document(), nullptr, this, nullptr);
}

void AgentMessageUnit::insertMarkdownWithCodeShadow(const QString &markdown)
{
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);

    // 如果当前已经有一段内容（例如思考内容），先新起一段，避免思考和回复挤在一起
    if (!document()->isEmpty())
        cursor.insertBlock();

    // 统一换行符，避免 Windows \r\n 影响解析
    QString text = markdown;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    // 匹配 ```语言\n代码\n```
    QRegularExpression fenceRe(
        QStringLiteral("```([\\w+#-]*)\\s*\\n([\\s\\S]*?)```")
    );

    int pos = 0;
    QRegularExpressionMatchIterator it = fenceRe.globalMatch(text);

    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();

        // 代码块前面的普通 Markdown
        const QString before = text.mid(pos, match.capturedStart() - pos);
        if (!before.trimmed().isEmpty()) {
            QStringList codes;
            const QString marked = replaceInlineCodeWithPlaceholders(before, codes);
            QTextDocument doc;
            doc.setMarkdown(marked);
            cursor.insertHtml(restoreInlineCodeHtml(doc.toHtml(), codes));
        }

        // 强制在代码块前新起一段，避免语言标签和普通文字挤在同一行
        cursor.insertBlock();

        // 自定义带阴影代码块
        const QString language = match.captured(1);
        const QString code = match.captured(2);
        const int insertPos = cursor.position();
        cursor.insertHtml(buildCodeShadowHtml(language, code));

        // 清除语言名段落的上下间距，避免语言名和代码之间出现空隙。
        // 只扫描本次插入的代码块区域，避免每个代码块都遍历整个文档。
        QTextBlock startBlock = document()->findBlock(insertPos);
        const QTextBlock endBlock = cursor.block();
        if (!startBlock.isValid())
            startBlock = document()->begin();
        for (QTextBlock block = startBlock;
             block.isValid() && (!endBlock.isValid() || block.position() <= endBlock.position());
             block = block.next()) {
            if (block.text().trimmed() == language) {
                QTextCursor blockCursor(block);
                QTextBlockFormat fmt = blockCursor.blockFormat();
                fmt.setTopMargin(0);
                fmt.setBottomMargin(0);
                blockCursor.setBlockFormat(fmt);
            }
        }

        // 强制新起一段，防止后续文字被阴影/布局盖住
        cursor.insertBlock();

        // 清除继承自代码块的背景色，避免后续普通文本被错误带上代码背景
        QTextBlockFormat clearFormat;
        cursor.setBlockFormat(clearFormat);

        pos = match.capturedEnd();
    }

    // 最后剩余部分
    const QString after = text.mid(pos);
    if (!after.trimmed().isEmpty()) {
        // 流式输出时可能遇到还没闭合的 ``` 代码块，先按代码块渲染，等闭合后下次会正常解析
        QRegularExpression unclosedFenceRe(
            QStringLiteral("```([\\w+#-]*)\\s*\\n([\\s\\S]*)$")
        );
        QRegularExpressionMatch unclosedMatch = unclosedFenceRe.match(after);

        if (unclosedMatch.hasMatch()) {
            const QString before = after.left(unclosedMatch.capturedStart());
            if (!before.trimmed().isEmpty()) {
                QStringList codes;
                const QString marked = replaceInlineCodeWithPlaceholders(before, codes);
                QTextDocument doc;
                doc.setMarkdown(marked);
                cursor.insertHtml(restoreInlineCodeHtml(doc.toHtml(), codes));
            }

            cursor.insertBlock();
            const QString language = unclosedMatch.captured(1);
            const QString code = unclosedMatch.captured(2);
            cursor.insertHtml(buildCodeShadowHtml(language, code));
            cursor.insertBlock();
        } else {
            QStringList codes;
            const QString marked = replaceInlineCodeWithPlaceholders(after, codes);
            QTextDocument doc;
            doc.setMarkdown(marked);
            cursor.insertHtml(restoreInlineCodeHtml(doc.toHtml(), codes));
        }
    }

    setTextCursor(cursor);
    ensureCursorVisible();

    updateHeightToContent();
}

void AgentMessageUnit::appendMarkdownWithCodeShadow(const QString &markdown)
{
    Segment segment;
    segment.type = Segment::Markdown;
    segment.text = markdown;
    m_segments.append(segment);

    // 只渲染新增的这一段，而不是每次 clear() 后全量重建，
    // 避免连续追加多段内容时变成 O(n^2) 的重复渲染。
    // 整段插入期间也先抑制高度重算，最后统一更新一次。
    m_rebuilding = true;
    insertMarkdownWithCodeShadow(markdown);
    m_rebuilding = false;
    updateHeightToContent();
}

void AgentMessageUnit::appendHtml(const QString &html)
{
    Segment segment;
    segment.type = Segment::Html;
    segment.text = html;
    m_segments.append(segment);

    insertHtml(html);
    updateHeightToContent();
}

void AgentMessageUnit::insertHtml(const QString &html)
{
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertHtml(html);
    setTextCursor(cursor);
    ensureCursorVisible();
}

void AgentMessageUnit::appendThinking(const QString &thinking)
{
    const int index = m_thinkingBlocks.size();
    m_thinkingBlocks.append({thinking, m_expandedThinkingIndices.contains(index)});

    Segment segment;
    segment.type = Segment::Thinking;
    segment.thinkingIndex = index;
    m_segments.append(segment);

    // 只渲染新增的思考块，避免把已有回复全部重绘一遍。
    insertThinking(index);
    updateHeightToContent();
}

void AgentMessageUnit::appendSeparator()
{
    Segment segment;
    segment.type = Segment::Html;
    segment.text = QStringLiteral("<br>");
    m_segments.append(segment);

    insertHtml(segment.text);
    updateHeightToContent();
}

void AgentMessageUnit::resetContent()
{
    m_thinkingBlocks.clear();
    m_segments.clear();
    clear(); // 清掉当前文档，再增量渲染新的内容
}

void AgentMessageUnit::appendStreamChunk(StreamSegment::Type type, const QString &content)
{
    if (!m_streamSegments.isEmpty() && m_streamSegments.last().type == type) {
        m_streamSegments.last().content += content;
    } else {
        StreamSegment segment;
        segment.type = type;
        segment.content = content;
        m_streamSegments.append(segment);
    }
}

void AgentMessageUnit::flushStream()
{
    if (m_streamSegments.isEmpty())
        return;

    resetContent();

    // 按原始顺序渲染：思考 → 工具调用 → 回复 → 再思考 → ...
    for (const StreamSegment &segment : m_streamSegments) {
        switch (segment.type) {
        case StreamSegment::Thinking:
            appendThinking(segment.content);
            break;
        case StreamSegment::Reply:
            appendMarkdownWithCodeShadow(segment.content);
            break;
        case StreamSegment::ToolCall:
        case StreamSegment::ToolResult:
            appendHtml(QStringLiteral("<br>") + segment.content);
            break;
        }
    }
}

void AgentMessageUnit::clearStreamSegments()
{
    m_streamSegments.clear();
}

void AgentMessageUnit::rebuild()
{
    m_rebuilding = true;
    clear();

    for (const Segment &segment : m_segments) {
        switch (segment.type) {
        case Segment::Thinking:
            insertThinking(segment.thinkingIndex);
            break;
        case Segment::Markdown:
            insertMarkdownWithCodeShadow(segment.text);
            break;
        case Segment::Html:
            insertHtml(segment.text);
            break;
        case Segment::StreamText:
            insertHtml(segment.text.toHtmlEscaped());
            break;
        }
    }

    m_rebuilding = false;
    updateHeightToContent();
}

void AgentMessageUnit::insertThinking(int index)
{
    if (index < 0 || index >= m_thinkingBlocks.size())
        return;

    const ThinkingBlock &block = m_thinkingBlocks.at(index);
    const QString preview = thinkingPreview(block.content);
    const QString arrow = block.expanded ? QStringLiteral("▼") : QStringLiteral("▶");

    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    if (!document()->isEmpty())
        cursor.insertBlock();

    const QString header = QStringLiteral(
        "<a href=\"dsh://thinking/%1\" style=\"color:#888; text-decoration:none;\">%2 思考：%3</a>")
        .arg(index)
        .arg(arrow, preview.toHtmlEscaped());
    cursor.insertHtml(header);

    if (block.expanded) {
        cursor.insertBlock();
        cursor.insertHtml(QStringLiteral(
            "<p style='color:#888;'><i>%1</i></p>")
            .arg(block.content.toHtmlEscaped()));
    }

    setTextCursor(cursor);
    ensureCursorVisible();
}

void AgentMessageUnit::toggleThinking(int index)
{
    if (index < 0 || index >= m_thinkingBlocks.size())
        return;

    m_thinkingBlocks[index].expanded = !m_thinkingBlocks[index].expanded;
    if (m_thinkingBlocks[index].expanded)
        m_expandedThinkingIndices.insert(index);
    else
        m_expandedThinkingIndices.remove(index);
    rebuild();
}

QString AgentMessageUnit::thinkingPreview(const QString &content) const
{
    QString preview = content.simplified();
    if (preview.length() > 60)
        preview = preview.left(60) + QStringLiteral("...");
    return preview;
}

void AgentMessageUnit::updateHeightToContent()
{
    // 整批重建过程中不逐段重算高度，等所有段插入完后再统一计算一次
    if (m_rebuilding)
        return;

    int textWidth = viewport()->width();
    if (textWidth <= 0)
        textWidth = width() - frameWidth() * 2 - 8;

    document()->setTextWidth(textWidth);

    const qreal docHeight = document()->documentLayout()->documentSize().height();
    const int verticalPadding = 16; // QSS padding: 8px top + 8px bottom
    const int frame = frameWidth() * 2;

    setFixedHeight(static_cast<int>(docHeight) + verticalPadding + frame);
}

QString AgentMessageUnit::buildCodeShadowHtml(const QString &language, const QString &code)
{
    const QString lang = language.isEmpty() ? QStringLiteral("code") : language;


    // 最初版阴影样式：直接使用 box-shadow
    return QString(
        "<div style='"
        "box-shadow:0 2px 8px rgba(0,0,0,0.06);"
        "border:1px solid #E5E7EB;"
        "border-radius:8px;"
        "background:#F8F9FB;"
        "margin:8px 0;"
        "padding:0;"
        "'>"

        // 语言名称栏
        "<p style='"
        "background:#F8F9FB;"
        "padding:4px 10px;"
        "font-family:Consolas,Menlo,monospace;"
        "font-size:12px;"
        "font-style:italic;"
        "color:#6B7280;"
        "border-bottom:1px solid #E5E7EB;"
        "'>%1</p>"

        // 代码内容
        "<pre style='"
        "margin:0;"
        "padding:10px;"
        "font-family:Consolas,Menlo,monospace;"
        "font-size:13px;"
        "color:#1F2937;"
        "white-space:pre-wrap;"
        "word-break:break-all;"
        "'>%2</pre>"

        "</div>"
    ).arg(lang.toHtmlEscaped(), CodeHighlighter::instance().highlight(lang, code));
}

QString AgentMessageUnit::replaceInlineCodeWithPlaceholders(const QString &markdown, QStringList &codes) const
{
    QRegularExpression inlineRe(QStringLiteral("`([^`]+)`"));

    QString output;
    int pos = 0;
    int index = 0;
    QRegularExpressionMatchIterator it = inlineRe.globalMatch(markdown);

    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();

        output += markdown.mid(pos, match.capturedStart() - pos);
        codes << match.captured(1);
        output += QStringLiteral("@@INLINE_CODE_%1@@").arg(index);

        ++index;
        pos = match.capturedEnd();
    }

    output += markdown.mid(pos);
    return output;
}

QString AgentMessageUnit::restoreInlineCodeHtml(const QString &html, const QStringList &codes) const
{
    QString result = html;

    for (int i = 0; i < codes.size(); ++i) {
        const QString token = QStringLiteral("@@INLINE_CODE_%1@@").arg(i);
        const QString span = QStringLiteral(
            "<span style='"
            "background:#F8F9FB;"
            "border:1px solid #E5E7EB;"
            "box-shadow:0 1px 2px rgba(0,0,0,0.05);"
            "border-radius:4px;"
            "padding:1px 4px;"
            "font-family:Consolas,Menlo,monospace;"
            "font-size:0.9em;"
            "color:#24292f;"
            "'>%1</span>"
        ).arg(codes.at(i).toHtmlEscaped());
        result.replace(token, span);
    }

    return result;
}


