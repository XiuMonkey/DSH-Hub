// ------------------------------------------------------------------
// TestMarkdownPreprocess.cpp
// ------------------------------------------------------------------
// 见 TestMarkdownPreprocess.h。
// ------------------------------------------------------------------

#include "TestMarkdownPreprocess.h"

#include "common/util/MarkdownPreprocess.h"

#include <QTextDocument>
#include <QTest>

// U+2028 行分隔符（prepareMarkdownForQt 的替换产物）
static const QChar kLineSeparator = QChar(0x2028);

void TestMarkdownPreprocess::brTagReplacedWithLineSeparator()
{
	const QString input = QStringLiteral("第一行<br>第二行");
	QCOMPARE(prepareMarkdownForQt(input),
		QStringLiteral("第一行") + kLineSeparator + QStringLiteral("第二行"));
}

void TestMarkdownPreprocess::brTagVariantsReplaced()
{
	QCOMPARE(prepareMarkdownForQt(QStringLiteral("a<br>b")),
		QStringLiteral("a") + kLineSeparator + QStringLiteral("b"));
	QCOMPARE(prepareMarkdownForQt(QStringLiteral("a<br/>b")),
		QStringLiteral("a") + kLineSeparator + QStringLiteral("b"));
	QCOMPARE(prepareMarkdownForQt(QStringLiteral("a<br />b")),
		QStringLiteral("a") + kLineSeparator + QStringLiteral("b"));
	QCOMPARE(prepareMarkdownForQt(QStringLiteral("a<br\t/>b")),
		QStringLiteral("a") + kLineSeparator + QStringLiteral("b"));
}

void TestMarkdownPreprocess::brTagIsCaseInsensitive()
{
	QCOMPARE(prepareMarkdownForQt(QStringLiteral("a<BR>b")),
		QStringLiteral("a") + kLineSeparator + QStringLiteral("b"));
	QCOMPARE(prepareMarkdownForQt(QStringLiteral("a<Br />b")),
		QStringLiteral("a") + kLineSeparator + QStringLiteral("b"));
}

void TestMarkdownPreprocess::noBrTagLeavesTextUnchanged()
{
	const QString input = QStringLiteral("| **审批机制** | ① 写文件 | ✅ 通过 |\n\n## 要点回顾\n\n- 普通文本");
	QCOMPARE(prepareMarkdownForQt(input), input);
	// 相似但不匹配的标签不应被误替换
	QCOMPARE(prepareMarkdownForQt(QStringLiteral("a<brunch>b")), QStringLiteral("a<brunch>b"));
	QCOMPARE(prepareMarkdownForQt(QStringLiteral("a&lt;br&gt;b")), QStringLiteral("a&lt;br&gt;b"));
}

void TestMarkdownPreprocess::inlineCodePlaceholderUntouched()
{
	// 客户端会把内联代码先替换成占位符，占位符本身不能被误伤
	const QString input = QStringLiteral("先看 @@INLINE_CODE_0@@ 再看 @@INLINE_CODE_1@@");
	QCOMPARE(prepareMarkdownForQt(input), input);
}

void TestMarkdownPreprocess::tableWithBrRendersAllRows()
{
	// 与真实会话中触发问题的消息同构：表格单元格里含 <br> 多行内容
	const QString markdown = QStringLiteral(
		"## 测试结果\n\n"
		"| 功能 | 流程 | 结果 |\n"
		"|------|------|------|\n"
		"| **审批机制** | ① 写工作区外文件 → 沙箱拒绝<br>② 升级重试 → 弹审批提示<br>③ 批准 → 文件创建成功 | ✅ 通过 |\n"
		"| **提问功能** | 弹出两个问题 → 你选择了\"删除它\" | ✅ 通过 |\n"
		"| **清理** | 删除测试文件 → 批准后已删除 | ✅ 通过 |\n");

	QTextDocument doc;
	doc.setMarkdown(prepareMarkdownForQt(markdown));
	const QString plain = doc.toPlainText();

	// 修复前：第一个 <br> 之后的内容、后续行、以及表格后的内容全部丢失
	QVERIFY2(plain.contains(QStringLiteral("② 升级重试 → 弹审批提示")),
		qPrintable(QStringLiteral("单元格内 <br> 之后的第二行丢失，实际输出：\n%1").arg(plain)));
	QVERIFY2(plain.contains(QStringLiteral("③ 批准 → 文件创建成功")),
		qPrintable(QStringLiteral("单元格内 <br> 之后的第三行丢失，实际输出：\n%1").arg(plain)));
	QVERIFY2(plain.contains(QStringLiteral("提问功能")), qPrintable(plain));
	QVERIFY2(plain.contains(QStringLiteral("清理")), qPrintable(plain));
}

void TestMarkdownPreprocess::tableWithBrKeepsContentAfterTable()
{
	const QString markdown = QStringLiteral(
		"| 功能 | 流程 | 结果 |\n"
		"|------|------|------|\n"
		"| **审批机制** | ① 拒绝<br>② 重试 | ✅ |\n\n"
		"## 要点回顾\n\n"
		"- **审批是\"一次一弹\"**：每次越权操作都会独立触发审批提示。\n\n"
		"两个机制都工作正常。");

	QTextDocument doc;
	doc.setMarkdown(prepareMarkdownForQt(markdown));
	const QString plain = doc.toPlainText();

	// 修复前：表格之后的标题、列表、结尾段落全部变成空块
	QVERIFY2(plain.contains(QStringLiteral("要点回顾")),
		qPrintable(QStringLiteral("表格之后的内容丢失，实际输出：\n%1").arg(plain)));
	QVERIFY2(plain.contains(QStringLiteral("一次一弹")), qPrintable(plain));
	QVERIFY2(plain.contains(QStringLiteral("两个机制都工作正常")), qPrintable(plain));
}

void TestMarkdownPreprocess::plainParagraphWithBrKeepsRest()
{
	// 普通段落里的 <br> 同样会让 Qt 丢弃其后内容，预处理后应保留
	const QString markdown = QStringLiteral("第一行<br>第二行<br/>第三行");

	QTextDocument doc;
	doc.setMarkdown(prepareMarkdownForQt(markdown));
	const QString plain = doc.toPlainText();

	QVERIFY2(plain.contains(QStringLiteral("第一行")), qPrintable(plain));
	QVERIFY2(plain.contains(QStringLiteral("第二行")),
		qPrintable(QStringLiteral("段落中 <br> 之后的内容丢失，实际输出：\n%1").arg(plain)));
	QVERIFY2(plain.contains(QStringLiteral("第三行")), qPrintable(plain));
}