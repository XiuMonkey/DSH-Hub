#pragma once

// ------------------------------------------------------------------
// ChatInputWidget.h
// ------------------------------------------------------------------
// 聊天输入区（仿原生 DSH composer 的卡片结构）：
//   ┌──────────────────────────────────────────────┐
//   │ 多行输入框（自动增高）                        │
//   ├──────────────────────────────────────────────┤
//   │ 控制行：模型/思考档位 chip ……………… 发送/中止 │
//   └──────────────────────────────────────────────┘
//   24 轮 · 366 步 | LLM 47m27s · 工具调用 23m43s | …   ← 卡片下方的小灰字
//
// 卡片本体的圆角/边框/背景来自 chat.qss 的 #inputCapsule（内层控件 m_capsule）；
// 控制行左右各留一个布局（tools / trailing），后续新增控制按键直接挂进去。
// 卡片下方那行小灰字是 SessionStatsLine（同类但单独声明，见下）。
// Enter 发送、Shift+Enter 换行。
//
// 为什么 ChatInputWidget 自己不是卡片，而是"卡片 + 小灰字"的竖排容器：
// 小灰字要落在卡片**外面**（原生 composer 就是这么排的），而它由本控件创建，
// 所以卡片本体下沉成一层内层控件，样式规则仍旧认 #inputCapsule（后代选择器，
// #inputCapsule QPlainTextEdit 之类不受影响）。
//
// 悬浮感：卡片外面还套了一层 ShadowPanel（m_capsuleShadow）画阴影。
// QSS 没有 box-shadow，而原版这张卡片是带 --dsw-shadow-lv2 的，所以阴影走绘制。
// 卡片自己的 QSS 规则一条都不用改。
// ------------------------------------------------------------------

#include "common/appearance/CardShadow.h"

#include <QString>
#include <QWidget>

class QHBoxLayout;
class QPlainTextEdit;
class QPushButton;
class ModelSelector;
class DshApiClient;
class ShadowPanel;

// ------------------------------------------------------------------
// 会话统计（卡片下方那行小灰字的数据）
// ------------------------------------------------------------------
// 字段直接对应服务端的两块会话投影（session/list 每行的 projections.values）：
//   sessionStats  @deepseek-ai/dsh-session-stats —— 轮/步 + LLM / 工具 / 首 token / 解码耗时
//   tokenUsage    @deepseek-ai/dsh-token-meter  —— 输入（未缓存 / 缓存读 / 缓存写）与输出 token
// 名字与语义跟官方 Web 端 composer 的 StatsLine 完全一致（见该包的
// StatsLine.d.ts / turn-metrics.d.ts），所以"服务端给什么就画什么"两边同一套规则。
// 服务端插件的注释说明：每个字段在它第一个事件到来前都是 0（key 一定存在），
// 因此这里也把 0 当"还没有"处理，而不是"真的是 0"。
struct SessionUsageStats
{
	// ---- sessionStats：整条日志的累计值，分页/压缩都不会改变它 ----
	int turns = 0;            // 轮：至少闭合了一个 step 的不同轮次
	int steps = 0;            // 步：step/end 计数（失败/取消的步也算）
	qint64 llmMs = 0;         // LLM 用时：step/start -> assistant/message 累加
	qint64 toolMs = 0;        // 工具调用用时：tool/call -> tool/result 按 callId 配对累加
	qint64 ttftMs = 0;        // 首 token 延迟累加；除以 ttftSteps 才是"平均"
	int ttftSteps = 0;        // 记到了首 token 的步数
	qint64 decodeMs = 0;      // 解码用时：首 token -> assistant/message
	qint64 decodeTokens = 0;  // 同一批步里服务端报的输出 token

	// ---- tokenUsage：整条日志的计费口径 ----
	bool hasUsage = false;              // 服务端还没给过 tokenUsage 时为 false
	qint64 uncachedInputTokens = 0;     // 未命中缓存的输入
	qint64 cacheReadTokens = 0;         // 命中缓存读回
	qint64 cacheWriteTokens = 0;        // 写入缓存
	qint64 outputTokens = 0;            // 输出
};

// ------------------------------------------------------------------
// SessionStatsLine —— 卡片下方那行小灰字
// ------------------------------------------------------------------
// 单独成类但不单独成文件：和输入框、发送按钮写在一块（本头文件 + ChatInputWidget.cpp）。
// 只做一件事：把 SessionUsageStats 拼成一行小灰字画出来，不关心数据怎么来。
//
//   * 固定 14px 行高 —— 输入区上下留白里那 14px 就是留给它的（见 Main.cpp）；
//   * 一行、居中、超出宽度用省略号，并把完整内容挂到 tooltip（对齐官方 StatsLine）；
//   * **始终显示**（与官方的一处有意差异）：官方在没有可显示内容时整行不渲染，
//     这里改成「轮/步」与「输入/输出」两组无条件出现 —— 新会话看到的是
//     `0 轮 · 0 步 | 输入 0 tok · 输出 0 tok`，而不是一片空白。
//     其余组没有有意义的 0 表示，仍为 0 就不出现（见 formatStats）。
//   * 底部与侧栏卡片底边齐平：输入区的下留白 = 侧栏阴影外壳的下留白（见 Main.cpp）。
//     有/无统计时行高不变，输入区不会上下跳。
//
// 数据从哪来、什么时候刷新，暂时写在 DSHHub.cpp 里（见 DSHHub::refreshSessionStats）。
class SessionStatsLine : public QWidget
{
	Q_OBJECT

public:
	explicit SessionStatsLine(QWidget* parent = nullptr);

	// 固定行高：输入区留白里那 14px 就是给它的
	static constexpr int kHeight = 14;

	// 按结构化统计重画
	void setStats(const SessionUsageStats& stats);
	// 直接给一整行文本（兜底 / 测试用）；空串表示这一行不画字
	void setLineText(const QString& line);
	// 回到"零状态"那一行（0 轮 · 0 步 | 输入 0 tok · 输出 0 tok）。
	// 换会话时用它清掉上一会话的数字，同时保住"始终显示"。
	void clearStats();

	// 当前这一行的完整文本
	QString lineText() const { return m_lineText; }

protected:
	void paintEvent(QPaintEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;

private:
	// 拼"轮 · 步 | LLM … | 缓存命中 … | 输入 … tok · 输出 … tok"整行；
	// 没有可显示内容时返回空串
	static QString formatStats(const SessionUsageStats& stats);

	// 只在文本被省略号截断时挂 tooltip（与官方 StatsLine 的 truncated 判定一致）
	void syncToolTip();

	QString m_lineText;
};

class ChatInputWidget : public QWidget
{
	Q_OBJECT

public:
	explicit ChatInputWidget(QWidget* parent = nullptr);

	// 输入卡片的阴影规格。Main.cpp 要拿它的四周留白反推输入区的边距
	// （边距 = 原边距 - 留白，卡片宽度才不会被阴影挤窄），所以放在这里两边共用，
	// 避免同一个数字在两处各写一份然后漂移。
	static CardShadow::Spec shadowSpec();

	// 获取当前输入内容（未 trim）
	QString text() const;

	// 清空输入框并复位高度
	void clear();

	// 切换发送按钮/中止输出按钮状态
	void setStreaming(bool streaming);

	// 绑定当前会话：底部“模型 / 思考深度”控件按该会话的模型目录刷新
	void setModelSession(DshApiClient* api, const QString& sessionId);

	// 服务端模型目录变了（例如刚在设置里新增了一个模型）后重新拉取一次
	void refreshModelCatalog();

	/**
	 * 用该会话记录过的模型选择覆盖 chip
	 * （见 ModelSelector::overrideCurrentSelection）。provider/model 为空表示
	 * 服务端还没记录过，保持目录给的部署默认值。
	 */
	void applySessionModelSelection(const QString& provider, const QString& model,
		const QString& reasoningEffort);

	// ---- 卡片下方的小灰字统计（会话统计；控件只认数据，怎么取数据在 DSHHub 里）----
	// 推一次统计：整行由 SessionStatsLine 自己拼
	void setSessionStats(const SessionUsageStats& stats);
	// 擦掉统计（例如没有当前会话）
	void clearSessionStats();

signals:
	// 点击发送按钮或按 Enter 时发出，携带当前输入框内容（未 trim）
	void sendRequested(const QString& text);
	// 当前会话正在输出时，点击中止按钮或按 Enter 发出
	void stopRequested();
	// 用户换了模型（服务端已接受）
	void modelChanged(const QString& provider, const QString& model);
	// 用户调整了思考深度（服务端已接受）
	void thinkingDepthChanged(const QString& levelId);

protected:
	bool eventFilter(QObject* obj, QEvent* event) override;
	// 语言切换后：输入框提示语与发送/中止按钮的提示要跟着换
	void changeEvent(QEvent* event) override;

private slots:
	void handleSendClicked();

private:
	// 输入框随内容自动增高
	void adjustHeight();

	// 根据悬停/按下状态显示发送按钮上方的圆形灰色蒙版
	void updateSendOverlay();

	// 按当前状态重设文案（构造末尾与语言切换时调用）
	void retranslateUi();

	// 搭建输入卡片本体（圆角/边框/背景由 #inputCapsule 画，内容挂 m_capsule 的布局）
	void buildCapsule();

	// 搭建底部控制行（模型/思考档位 chip + 发送键）
	void buildControlRow();

	// 卡片本体（objectName = inputCapsule）：输入框 + 控制行都在它里面
	QWidget* m_capsule = nullptr;
	// 卡片下方的小灰字统计行
	SessionStatsLine* m_statsLine = nullptr;

	QPlainTextEdit* m_editor = nullptr;
	QPushButton* m_sendButton = nullptr;
	QWidget* m_sendOverlay = nullptr;
	QWidget* m_controlRow = nullptr;
	QHBoxLayout* m_toolsLayout = nullptr;
	QHBoxLayout* m_trailingLayout = nullptr;
	ModelSelector* m_modelSelector = nullptr;
	bool m_sendHovered = false;
	bool m_sendPressed = false;
	bool m_streaming = false;
};
