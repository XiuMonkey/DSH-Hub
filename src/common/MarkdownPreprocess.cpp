// ------------------------------------------------------------------
// MarkdownPreprocess.cpp
// ------------------------------------------------------------------
// 见 MarkdownPreprocess.h。
// ------------------------------------------------------------------

#include "MarkdownPreprocess.h"

#include <QRegularExpression>

QString prepareMarkdownForQt(const QString& markdown)
{
	QString result = markdown;
	// Qt 的 Markdown 导入器不支持 <br> 标签：一旦遇到 <br>，导入器会丢弃其后
	// 同一行/单元格的所有内容（表格单元格被截断、后续表格行及表格之后的
	// 整段内容全部丢失）。用 Unicode 行分隔符 U+2028 替换 <br>，
	// Qt 会把 U+2028 渲染成真正的换行。
	result.replace(
		QRegularExpression(QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption),
		QStringLiteral("\u2028"));
	return result;
}