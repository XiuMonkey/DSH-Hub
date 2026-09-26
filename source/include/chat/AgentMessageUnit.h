#pragma once

// Agent 消息气泡：垂直布局的“部件流”，同一气泡内正文与代码块严格按出现顺序排布。每段正文一个
// QTextBrowser（objectName=agentProse）；代码围栏切成独立 CodeBlockView（不换行、自带滚动、上方带语言标签）；
// Thinking/Tool 折叠块走 dsh:// 锚点，锚点宿主见 proseHost()。

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

class AgentMessageUnit : public QWidget
{
	Q_OBJECT

public:
	static constexpr int DefaultWidth = 720;

	explicit AgentMessageUnit(QWidget* parent = nullptr);
	~AgentMessageUnit() override;

	// 追加 Markdown（含代码阴影）；合并多段输出时用 appendSeparator 插入段落分隔、明确换行
	void appendMarkdownWithCodeShadow(const QString& markdown);
	void appendSeparator();
	void appendThinking(const QString& thinking);
	void appendToolCall(const QString& name, const QString& argumentsHtml);
	void appendToolResult(const QString& resultHtml);
	void appendStreamChunk(StreamSegment::Type type, const QString& content, const QString& toolName = QString());
	void flushStream();
	void clearStreamSegments();

	// 对全部内容子部件做一次高度刷新（可安全对外调用）
	void updateHeightToContent();

	// 批量渲染模式：append* 期间不逐次高度拟合/排队 refit，整批结束后由构建方关闭并统一 updateHeightToContent()
	void setBulkFit(bool bulk) { m_bulkFit = bulk; }

	// 是否已渲染任何内容（正文/代码/思考/工具任意）；供 MessageQuery 判断上一气泡是否为空
	bool hasContent() const;

	// 气泡纯文本：各 ProseView 的 toPlainText + 各代码块文本按顺序拼接，段间换行
	QString textContent() const;

protected:
	void resizeEvent(QResizeEvent* event) override;

private:
	struct ThinkingBlock
	{
		QString content;
		bool expanded = false;
		// 布局中的卡片与其展开正文；就地展开/收起，避免整条 rebuild
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

	QVBoxLayout* m_partsLayout = nullptr;
	// 每段普通文本一个 ProseView；dsh:// 锚点宿主也在其中（清空见 clearParts）
	QList<QTextBrowser*> m_proseViews;

	QList<ThinkingBlock> m_thinkingBlocks;
	QList<ToolBlock> m_toolBlocks;
	QList<Segment> m_segments;
	QList<StreamSegment> m_streamSegments;
	QSet<int> m_expandedThinkingIndices; // 流式重建时保留用户展开状态
	QSet<int> m_expandedToolIndices; // 流式重建时保留工具块展开状态

	bool m_rebuilding = false;
	bool m_bulkFit = false; // 批量渲染期间跳过逐次高度拟合（见 setBulkFit）

	// 流式去重：最近一次已渲染的流式内容指纹，无变化时跳过整段重建
	QString m_lastFlushedFingerprint;
	// m_streamSegments 的内容指纹（类型 + 内容哈希）
	QString streamFingerprint() const;

	// 流式增量渲染：只重画“正在增长的最后一段”，不整条清空重建；已封闭段由既有 append* 一次性插入
	int m_streamSealedCount = 0; // 已渲染完的“封闭段”数量（不含 live 段）
	int m_liveIndex = -1; // live Reply 在 m_streamSegments 的下标；-1=无
	int m_liveLayoutMark = -1; // live 区域在 m_partsLayout 中的起始项下标
	int m_liveSegmentEntry = -1; // live Reply 在 m_segments 里对应的 Markdown 段下标
	QString m_liveRenderedText; // 上次已渲染的 live Reply 全文（未变则跳过）
	int m_liveThinkingSegment = -1; // live Thinking 在 m_streamSegments 的下标；-1=无
	int m_liveThinkingBlock = -1; // live Thinking 对应 m_thinkingBlocks 的下标；-1=未建卡

	// 排版诊断（DSH_HUB_LAYOUT_TRACE=1）：打印本气泡及子部件的高度/宽度/尺寸提示
	void debugTraceLayout(const QString& stage) const;

	// 重建 live 区域：删掉该区域旧部件，按最新全文重新渲染
	void renderLiveReply(const QString& markdown);
	// 思考段随 token 流式增长：首次建卡，之后原地刷新同一张卡
	void updateLiveThinking(const QString& content);
	// 封闭 live 思考段：保留卡片并标记已封存，避免重复追加
	void sealLiveThinking();
	// 结束 live 区域：只清标记，不删除部件
	void closeLiveReply();
	// 重置流式增量渲染的簿记（resetContent / clearStreamSegments 时调用）
	void resetLiveState();
	// 整条 rebuild 后把 live 标记对齐到新尾部，防止后续 flush 误删旧区域
	void syncLiveAfterRebuild();

	void rebuild();
	void insertMarkdownWithCodeShadow(const QString& markdown);
	void insertHtml(const QString& html);
	void insertThinking(int index);
	void toggleThinking(int index);
	void insertTool(int index);
	void toggleTool(int index);
	QString thinkingPreview(const QString& content) const;

	// 清空 m_partsLayout 的所有子部件并清空 m_proseViews（用于 rebuild / resetContent）
	void clearParts();
	// 处理 ProseView 上的 dsh:// 锚点点击（思考/工具切换）
	void handleAnchorClicked(const QUrl& url);
	// 新建一个空 ProseView 并加入布局尾部
	QTextBrowser* makeProseView();
	// 可追加尾部内容（分隔符/锚点）的宿主 ProseView：末尾部件本身是 ProseView 就用它，否则在布局末尾新建一个
	QTextBrowser* proseHost();
	// 把一段 Markdown 渲染进一个全新的 ProseView（代码围栏已在外层切开）
	void appendProseRegion(const QString& markdown);
	// 追加一个代码子单元：语言小标签 + CodeBlockView（一起作为一个布局子部件）
	void addCodeBlockUnit(const QString& language, const QString& code);
	// 把该 ProseView 的固定高度设为文档高度
	void fitProseView(QTextBrowser* view);
	void refitParts();

	// 配置并返回“可自适高”的富文本子控件（透明、无边框、换行、锚点回调）；不加入布局、不登记，由调用方决定
	QTextBrowser* createRichPart(const QString& objectName);
	void addThinkingCard(int index);
	void addToolCard(int index);
	// 就地展开/收起思考卡（不整条 rebuild，避免跳动/抖动）
	void updateThinkingCard(int index);
	// 就地展开/收起工具卡（不整条 rebuild，避免跳动/抖动）
	void updateToolCard(int index);

	// 把 Markdown 行内代码替换成占位符，避免 Qt 解析丢失样式
	QString replaceInlineCodeWithPlaceholders(const QString& markdown, QStringList& codes) const;
	QString restoreInlineCodeHtml(const QString& html, const QStringList& codes) const;
};
