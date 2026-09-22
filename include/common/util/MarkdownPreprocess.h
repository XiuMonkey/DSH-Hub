#pragma once

// ------------------------------------------------------------------
// MarkdownPreprocess.h
// ------------------------------------------------------------------
// 在把 Markdown 交给 Qt 的 QTextDocument::setMarkdown() 之前做预处理，
// 规避 Qt 6 Markdown 导入器（QTextMarkdownImporter）不支持 <br> 的问题。
// ------------------------------------------------------------------

#include <QString>

// 把 <br> / <br/> / <br />（大小写不敏感）替换为 Unicode 行分隔符 U+2028。
//
// Qt 6 的 Markdown 导入器不支持 <br> 标签：一旦在内容里遇到 <br>，
// 它会丢弃其后同一行/单元格的所有内容——表格单元格被截断、后续表格行
// 以及表格之后的整段内容全部丢失。QTextDocument 会把 U+2028 渲染成真正
// 的换行，因此替换后既保留了换行语义，又不会破坏表格解析。
QString prepareMarkdownForQt(const QString& markdown);
