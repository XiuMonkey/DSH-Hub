// 交给 QTextDocument::setMarkdown() 之前的 Markdown 预处理：绕开 Qt 6 导入器不支持 <br> 的坑。

#include "common/util/MarkdownPreprocess.h"

#include <QRegularExpression>

QString prepareMarkdownForQt(const QString& markdown)
{
	QString result = markdown;
	// Qt 的 Markdown 导入器不支持 <br>：一旦遇到就丢弃其后同一行/单元格的所有内容
	// （表格单元格被截断、后续表格行及表格之后的整段内容全部丢失）。用 Unicode 行分隔符
	// U+2028 替换 <br>，Qt 会把它渲染成真正的换行。
	result.replace(QRegularExpression(QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption),
		QStringLiteral("\u2028"));
	return result;
}
