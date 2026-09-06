#pragma once

// ------------------------------------------------------------------
// AgentMessageUnit.h
// ------------------------------------------------------------------
// 可复用的 Agent 消息展示控件（气泡容器化 · 路线2 第2步）。
//
// 旧实现是单个 QTextBrowser 文档，Markdown 代码围栏以 HTML 阴影卡渲染进同一篇文档；
// 现改为 QWidget + 垂直布局承载的“部件流”，同一气泡里普通文本与代码块严格按出现
// 顺序排布：
//   - 每“一段普通文本”用一个 QTextBrowser（objectName=agentProse）：只读、透明
//     无边框、富文本换行、高度自适应；
//   - Markdown 代码围栏切成独立的 CodeBlockView 子控件（不换行、自带横/纵滚动，
//     外观走 chat.qss 的 QPlainTextEdit#codeBlockView），上方带语言小标签
//     （objectName=codeBlockLang）；
//   - Thinking / Tool 可折叠块沿用 dsh:// 富文本锚点方案，锚点挂到消息尾部对应的
//     ProseView 文档里（宿主选择见 proseHost() 的注释）。
// ------------------------------------------------------------------

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

class QTextBrowser;
class QVBoxLayout;
class QUrl;
class QResizeEvent;

// 流式消息片段，按出现顺序保存，保证思考/工具调用/回复交错显示
struct StreamSegment
{
	enum Type { Thinking, Reply, ToolCall, ToolResult };
	Type type = Reply;
	QString content;
	QString toolName; // 仅 ToolCall 使用
};

/**
 * 可复用的 Agent 消息展示控件。
 *
 * 使用示例：
 * @code
 * AgentMessageUnit *view = new AgentMessageUnit;
 * view->setFixedWidth(720);
 * view->appendMarkdownWithCodeShadow(reply);
 * @endcode
 */
class AgentMessageUnit : public QWidget
{
	Q_OBJECT

public:
	/** 默认气泡宽度。 */
	static constexpr int DefaultWidth = 720;

	explicit AgentMessageUnit(QWidget* parent = nullptr);
	~AgentMessageUnit() override;

	/** 追加一段带代码块的 Markdown 渲染内容（普通文本进 ProseView，围栏进 CodeBlockView）。 */
	void appendMarkdownWithCodeShadow(const QString& markdown);

	/** 在已有内容后面插入一个段落分隔，用于合并多段输出时明确换行。 */
	void appendSeparator();

	/** 追加一段可折叠思考内容。 */
	void appendThinking(const QString& thinking);

	/** 追加一个可折叠的工具调用块。 */
	void appendToolCall(const QString& name, const QString& argumentsHtml);

	/** 追加一个可折叠的工具结果块。 */
	void appendToolResult(const QString& resultHtml);

	/** 清空已渲染内容以及内部记录的 segment/思考块，准备重新渲染整段内容。 */
	void resetContent();

	/** 追加一个流式片段。 */
	void appendStreamChunk(StreamSegment::Type type, const QString& content,
		const QString& toolName = QString());

	/** 把累积的流式片段渲染到当前气泡。 */
	void flushStream();

	/** 清空流式片段缓存。 */
	void clearStreamSegments();

	/** 对当前所有内容子部件做一次高度刷新（对外保持安全）。 */
	void updateHeightToContent();

	/** 是否已渲染任何内容（正文/代码/思考/工具任意），供 MessageQuery 判断上一气泡是否为空。 */
	bool hasContent() const;

	/** 气泡的纯文本：各 ProseView 的 toPlainText + 各代码块文本按顺序拼接，段间换行。 */
	QString textContent() const;

protected:
	void resizeEvent(QResizeEvent* event) override;

private:
	struct ThinkingBlock
	{
		QString content;
		bool expanded = false;
		// 卡片化后：当前布局中的卡片与其展开正文（就地展开/收起用，避免整条 rebuild）
		QWidget* card = nullptr;
		QTextBrowser* body = nullptr;
	};

	struct ToolBlock
	{
		QString title;
		QString content;
		bool expanded = false;
		QWidget* card = nullptr;
		QTextBrowser* body = nullptr;
	};

	struct Segment
	{
		enum Type
		{
			Thinking,
			Tool,
			Markdown,
			Html
		};
		Type type = Markdown;
		int thinkingIndex = -1;
		int toolIndex = -1;
		QString text;
	};

	// —— 气泡容器化后的成员 ——
	QVBoxLayout* m_partsLayout = nullptr; // 垂直“部件流”：ProseView / 代码子单元按顺序排列
	QList<QTextBrowser*> m_proseViews;    // 每次“一段普通文本”用一个（锚点宿主也在其中）

	QList<ThinkingBlock> m_thinkingBlocks;
	QList<ToolBlock> m_toolBlocks;
	QList<Segment> m_segments;
	QList<StreamSegment> m_streamSegments;
	QSet<int> m_expandedThinkingIndices; // 流式重建时保留用户展开状态
	QSet<int> m_expandedToolIndices; // 流式重建时保留工具块展开状态

	bool m_rebuilding = false;

	// 流式去重：最近一次已渲染的流式内容指纹（无变化时跳过整段重建）
	QString m_lastFlushedFingerprint;
	// 计算 m_streamSegments 的内容指纹（类型 + 内容哈希）
	QString streamFingerprint() const;

	void rebuild();
	void insertMarkdownWithCodeShadow(const QString& markdown);
	void insertHtml(const QString& html);
	void insertThinking(int index);
	void toggleThinking(int index);
	void insertTool(int index);
	void toggleTool(int index);
	QString thinkingPreview(const QString& content) const;

	// —— 部件流内部辅助 ——
	// 清空 m_partsLayout 的所有子部件并清空 m_proseViews（用于 rebuild / resetContent）
	void clearParts();
	// 处理 ProseView 上的 dsh:// 锚点点击（思考/工具切换）
	void handleAnchorClicked(const QUrl& url);
	// 新建一个空 ProseView 并加入布局尾部
	QTextBrowser* makeProseView();
	// 返回可以追加“尾部内容”（分隔符）的宿主 ProseView：
	// 若消息末尾部件本身就是 ProseView，用它；否则（消息为空，或以代码块结尾）在布局
	// 末尾新建一个空 ProseView 作为宿主，保证追加内容总是出现在消息末尾之后。
	QTextBrowser* proseHost();
	// 把一段 Markdown 渲染进一个全新的 ProseView（代码围栏已在外层切开）
	void appendProseRegion(const QString& markdown);
	// 追加一个代码子单元：语言小标签 + CodeBlockView（一起作为一个布局子部件）
	void addCodeBlockUnit(const QString& language, const QString& code);
	// 把某个 ProseView 的固定高度设为文档高度
	void fitProseView(QTextBrowser* view);
	// 对当前所有 ProseView 重新拟合高度
	void refitParts();

	// —— Step4：Thinking / Tool 卡片化 ——
	// 配置并返回一个“可自适高”的富文本子控件（透明、无边框、换行、锚点回调；
	// 不加入布局、不登记，由调用方决定）
	QTextBrowser* createRichPart(const QString& objectName);
	// 追加一个可折叠的“思考”卡片子部件
	void addThinkingCard(int index);
	// 追加一个可折叠的“工具”卡片子部件
	void addToolCard(int index);
	// 就地展开/收起思考卡（不整条 rebuild，避免跳动/抖动）
	void updateThinkingCard(int index);
	// 就地展开/收起工具卡（不整条 rebuild，避免跳动/抖动）
	void updateToolCard(int index);

	/** 把 Markdown 行内代码替换成占位符，避免 Qt 解析丢失样式。 */
	QString replaceInlineCodeWithPlaceholders(const QString& markdown, QStringList& codes) const;

	/** 把占位符恢复成带样式的行内代码 HTML。 */
	QString restoreInlineCodeHtml(const QString& html, const QStringList& codes) const;
};
