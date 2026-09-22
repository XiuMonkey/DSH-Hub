#pragma once

// 气泡容器内的独立“代码块”控件：只读、可选中复制、支持语法高亮 HTML、不自动换行（单行过长出横向滚动条），
// 高度随行数自动增长，超过上限后固定在上限并启用纵向滚动；外观（灰底/边框/圆角）由 chat.qss 的 #codeBlockView 绘制。

#include <QTextEdit>

class CodeBlockView : public QTextEdit
{
public:
	static constexpr int DefaultMaxHeight = 320;

	explicit CodeBlockView(QWidget* parent = nullptr);

	void setCodeHtml(const QString& html);

	// 内容变化后重新计算高度；外部也可调用
	void updateHeightToContent();

private:
	int m_maxHeight = DefaultMaxHeight;
};
