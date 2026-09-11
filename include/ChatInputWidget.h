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
// 卡片本体的圆角/边框/背景来自 chat.qss 的 #inputCapsule；
// 控制行左右各留一个布局（tools / trailing），后续新增控制按键直接挂进去。
// Enter 发送、Shift+Enter 换行。
// ------------------------------------------------------------------

#include <QString>
#include <QWidget>

class QHBoxLayout;
class QPlainTextEdit;
class QPushButton;
class ModelSelector;
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

	// 绑定当前会话：底部“模型 / 思考深度”控件按该会话的模型目录刷新
	void setModelSession(DshApiClient* api, const QString& sessionId);

	// 服务端模型目录变了（例如刚在设置里新增了一个模型）后重新拉取一次
	void refreshModelCatalog();

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

	// 搭建底部控制行（模型/思考档位 chip + 发送键）
	void buildControlRow();

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
