#pragma once

// ------------------------------------------------------------------
// CodeBlockView.h
// ------------------------------------------------------------------
// 气泡容器内的“代码块”独立查看控件（路线 2）：
//   - 只读、可选中复制；
//   - 支持语法高亮 HTML（配合 CodeHighlighter 的输出）；
//   - 不自动换行（NoWrap），单行过长时出现横向滚动条；
//   - 高度随行数自动增长，超过上限后固定在上限并启用纵向滚动。
// 外观（背景/边框/圆角，灰色底）由 chat.qss 里 #codeBlockView 统一绘制。
// ------------------------------------------------------------------

#include <QTextEdit>

class CodeBlockView : public QTextEdit
{
public:
	/** 代码块高度上限：内容超出后启用纵向滚动。 */
	static constexpr int DefaultMaxHeight = 320;

	explicit CodeBlockView(QWidget* parent = nullptr);

	/** 设置语法高亮后的 HTML 代码并自动调整高度。 */
	void setCodeHtml(const QString& html);

	/** 内容变化后重新计算高度（外部也可调用）。 */
	void updateHeightToContent();

private:
	int m_maxHeight = DefaultMaxHeight;
};
