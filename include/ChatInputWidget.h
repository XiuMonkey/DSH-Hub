#pragma once

// ------------------------------------------------------------------
// ChatInputWidget.h
// ------------------------------------------------------------------
// 聊天输入区（仿原生 DSH composer 的卡片结构）：
//   ┌──────────────────────────────────────────────┐
//   │ 多行输入框（自动增高）                        │
//   ├──────────────────────────────────────────────┤
//   │ 控制行：思考深度 chip ……………… 发送/中止按钮  │
//   └──────────────────────────────────────────────┘
// 卡片本体的圆角/边框/背景来自 chat.qss 的 #inputCapsule；
// 控制行左右各留一个布局（tools / trailing），后续新增控制按键直接挂进去。
// Enter 发送、Shift+Enter 换行。
// ------------------------------------------------------------------

#include <QString>
#include <QWidget>

class QHBoxLayout;
class QPlainTextEdit;
class QPushButton;
class ThinkingDepthSelector;
class DshApiClient;

class ChatInputWidget : public QWidget
{
	Q_OBJECT

public:
	explicit ChatInputWidget(QWidget* parent = nullptr);

	// 获取当前输入内容（未 trim）
	QString text() const;

	// 清空输入框并复位高度
	void clear();

	// 切换发送按钮/中止输出按钮状态
	void setStreaming(bool streaming);

	// 绑定当前会话：底部“思考深度”控件按该会话的模型目录刷新
	void setThinkingSession(DshApiClient* api, const QString& sessionId);

signals:
	// 点击发送按钮或按 Enter 时发出，携带当前输入框内容（未 trim）
	void sendRequested(const QString& text);
	// 当前会话正在输出时，点击中止按钮或按 Enter 发出
	void stopRequested();
	// 用户调整了思考深度（服务端已接受）
	void thinkingDepthChanged(const QString& levelId);

protected:
	bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
	void handleSendClicked();

private:
	// 输入框随内容自动增高
	void adjustHeight();

	// 根据悬停/按下状态显示发送按钮上方的圆形灰色蒙版
	void updateSendOverlay();

	// 搭建底部控制行（思考深度 chip + 发送键）
	void buildControlRow();

	QPlainTextEdit* m_editor = nullptr;
	QPushButton* m_sendButton = nullptr;
	QWidget* m_sendOverlay = nullptr;
	QWidget* m_controlRow = nullptr;
	QHBoxLayout* m_toolsLayout = nullptr;
	QHBoxLayout* m_trailingLayout = nullptr;
	ThinkingDepthSelector* m_thinkingDepth = nullptr;
	bool m_sendHovered = false;
	bool m_sendPressed = false;
	bool m_streaming = false;
};
