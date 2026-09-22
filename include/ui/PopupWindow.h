#pragma once

// 无系统边框的现代弹出窗口：无 Windows 原生边框、圆角、白底、灰色细边框，右上角关闭按钮。

#include <QWidget>

class QCloseEvent;
class QEvent;
class QLabel;
class QPushButton;
class QVBoxLayout;

class PopupWindow : public QWidget
{
	Q_OBJECT

public:
	explicit PopupWindow(QWidget* parent = nullptr);

	void setTitle(const QString& title);
	void setContent(QWidget* content);

signals:
	void closed();

protected:
	void closeEvent(QCloseEvent* event) override;
	// 语言切换后：标题与关闭按钮提示要跟着换
	void changeEvent(QEvent* event) override;

private:
	void retranslateUi();

	QLabel* m_titleLabel = nullptr;
	QPushButton* m_closeButton = nullptr;
	QVBoxLayout* m_mainLayout = nullptr;
	QVBoxLayout* m_contentLayout = nullptr;
	// 记住标题：空标题时显示可翻译的默认名，切换语言要重算
	QString m_title;
};
