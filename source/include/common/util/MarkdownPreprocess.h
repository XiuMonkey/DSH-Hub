#pragma once

// 交给 QTextDocument::setMarkdown() 之前做的 Markdown 预处理：绕开 Qt 6 导入器不支持 <br> 的坑。

#include <QString>

// 把 <br> / <br/> / <br />（大小写不敏感）替换为 Unicode 行分隔符 U+2028：Qt 6 导入器遇到 <br> 会丢弃其后同一行/单元格的所有内容（表格被截断、后续整段丢失），而 U+2028 会被渲染成真正的换行且不破坏表格解析。
QString prepareMarkdownForQt(const QString& markdown);
