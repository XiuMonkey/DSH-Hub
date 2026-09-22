#pragma once

// ------------------------------------------------------------------
// TestMarkdownPreprocess.h
// ------------------------------------------------------------------
// MarkdownPreprocess 预处理及 Qt setMarkdown 渲染管线的单元测试，
// 覆盖“表格单元格内 <br> 导致内容丢失”的回归场景。
// ------------------------------------------------------------------

#include <QObject>
#include <QString>

class TestMarkdownPreprocess : public QObject
{
	Q_OBJECT

private slots:
	// prepareMarkdownForQt 纯字符串变换
	void brTagReplacedWithLineSeparator();
	void brTagVariantsReplaced();
	void brTagIsCaseInsensitive();
	void noBrTagLeavesTextUnchanged();
	void inlineCodePlaceholderUntouched();

	// 完整渲染管线回归：setMarkdown(prepareMarkdownForQt(...))
	void tableWithBrRendersAllRows();
	void tableWithBrKeepsContentAfterTable();
	void plainParagraphWithBrKeepsRest();
};
