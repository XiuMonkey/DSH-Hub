#pragma once

// 聊天输入区：输入卡片（内层控件 objectName = inputCapsule，QSS 按它匹配）+ 卡片下方的小灰字统计行；
// 卡片外面再套一层 ShadowPanel 画阴影 —— QSS 没有 box-shadow，阴影只能走绘制。
// Enter 发送、Shift+Enter 换行。

#include "common/appearance/CardShadow.h"

#include <QString>
#include <QWidget>

class QHBoxLayout;
class QPlainTextEdit;
class QPushButton;
class ModelSelector;
class DshApiClient;
class ShadowPanel;

// 小灰字的原始数据：对应服务端 sessionStats / tokenUsage 两块投影（session/list 每行的
// projections.values），字段语义同官方 Web 端 StatsLine；每字段在首个事件前都是 0，
// 所以 0 = “还没有”，不是“真的是 0”。
struct SessionUsageStats
{
	// 轮：至少闭合了一个 step 的不同轮次；步：step/end 计数（失败/取消的步也算）
	int turns = 0;
	int steps = 0;
	// LLM 用时 step/start -> assistant/message；工具用时 tool/call -> tool/result 按 callId 配对
	qint64 llmMs = 0;
	qint64 toolMs = 0;
	// 首 token 延迟累加（除以 ttftSteps 才是“平均”）；ttftSteps 记到了首 token 的步数
	qint64 ttftMs = 0;
	int ttftSteps = 0;
	// 解码用时 首 token -> assistant/message；decodeTokens 为同一批步里服务端报的输出 token
	qint64 decodeMs = 0;
	qint64 decodeTokens = 0;
	// 服务端还没给过 tokenUsage 时为 false
	bool hasUsage = false;
	qint64 uncachedInputTokens = 0;
	qint64 cacheReadTokens = 0;
	qint64 cacheWriteTokens = 0;
	qint64 outputTokens = 0;
};

// 卡片下方那行小灰字：固定 14px 行高（输入区留白里那 14px 就是给它的），始终显示
//（与官方“无内容整行不渲染”是有意差异）
class SessionStatsLine : public QWidget
{
	Q_OBJECT

public:
	explicit SessionStatsLine(QWidget* parent = nullptr);

	// 输入区留白里那 14px 就是给它的
	static constexpr int kHeight = 14;

	void setStats(const SessionUsageStats& stats);
	// 直接给整行文本（兜底 / 测试用）；空串表示这一行不画字
	void setLineText(const QString& line);
	// 回到零状态行；换会话时用它清掉上一会话的数字，同时保住“始终显示”
	void clearStats();

protected:
	void paintEvent(QPaintEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;

private:
	// 拼整行；没有可显示内容时返回空串
	static QString formatStats(const SessionUsageStats& stats);
	// 只在文本被省略号截断时挂 tooltip
	void syncToolTip();

	QString m_lineText;
};

class ChatInputWidget : public QWidget
{
	Q_OBJECT

public:
	explicit ChatInputWidget(QWidget* parent = nullptr);

	// 阴影规格：Main.cpp 要拿它的四周留白反推输入区边距（边距 = 原边距 - 留白），两处共用免得漂移
	static CardShadow::Spec shadowSpec();

	// 当前输入内容（未 trim）
	QString text() const;
	void clear();

	void setStreaming(bool streaming);

	// 绑定会话：底部“模型 / 思考深度”控件按该会话的模型目录刷新
	void setModelSession(DshApiClient* api, const QString& sessionId);

	// 服务端模型目录变了（例如刚在设置里新增了模型）后重拉一次
	void refreshModelCatalog();

	// 用该会话记录过的模型选择覆盖 chip；provider/model 为空表示服务端还没记录，保持目录给的部署默认值
	void applySessionModelSelection(const QString& provider, const QString& model, const QString& reasoningEffort);

	// 卡片下方的小灰字统计行（控件只认数据，怎么取数据在 DSHHub 里）
	void setSessionStats(const SessionUsageStats& stats);
	void clearSessionStats();

signals:
	// 点发送键或回车；text 未 trim
	void sendRequested(const QString& text);
	// 正在输出时点中止键或回车
	void stopRequested();
	// 用户换了模型（服务端已接受）
	void modelChanged(const QString& provider, const QString& model);
	// 用户调整了思考深度（服务端已接受）
	void thinkingDepthChanged(const QString& levelId);

protected:
	bool eventFilter(QObject* obj, QEvent* event) override;
	// 语言切换后重设输入框提示语与发送/中止按钮提示
	void changeEvent(QEvent* event) override;

private slots:
	void handleSendClicked();

private:
	void adjustHeight();
	// 按悬停/按下状态显示发送键上方的圆形灰色蒙版
	void updateSendOverlay();
	// 构造末尾与语言切换时调用
	void retranslateUi();
	// 搭卡片本体：圆角/边框/背景由 #inputCapsule 画，内容挂 m_capsule 的布局
	void buildCapsule();
	void buildControlRow();

	// 卡片本体，objectName 必须是 inputCapsule（QSS 按它匹配）；输入框与控制行都在它里面
	QWidget* m_capsule = nullptr;
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
