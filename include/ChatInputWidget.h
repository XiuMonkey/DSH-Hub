#pragma once

// ------------------------------------------------------------------
// ChatInputWidget.h
// ------------------------------------------------------------------
// 自动增高聊天输入框 + 附加发送按钮。
// 内部包含圆角输入胶囊、自动换行/增高、Enter 发送、Shift+Enter 换行。
// ------------------------------------------------------------------

#include <QString>
#include <QWidget>

class QPlainTextEdit;
class QPushButton;

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

signals:
	// 点击发送按钮或按 Enter 时发出，携带当前输入框内容（未 trim）
	void sendRequested(const QString& text);
	// 当前会话正在输出时，点击中止按钮或按 Enter 发出
	void stopRequested();

protected:
	bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
	void handleSendClicked();

private:
	// 输入框随内容自动增高
	void adjustHeight();

	// 根据悬停/按下状态显示发送按钮上方的圆形灰色蒙版
	void updateSendOverlay();

	QPlainTextEdit* m_editor = nullptr;
	QPushButton* m_sendButton = nullptr;
	QWidget* m_sendOverlay = nullptr;
	bool m_sendHovered = false;
	bool m_sendPressed = false;
	bool m_streaming = false;
};
