#pragma once

// 无系统边框的现代弹出窗口：无 Windows 原生边框、圆角、白底、灰色细边框，右上角关闭按钮。
// 常驻对象（不随关闭销毁）的"开/关/同步遮罩"三件套也由这里统一实现，见 openHosted。

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

	// 铺遮罩 + 居中 + 显示自己：背靠背完成，两者落在同一帧。已在显示时忽略重复请求。
	void openHosted();
	// 先收遮罩、再隐藏自己（常驻：不销毁，供下次打开复用）。
	void closeHosted();
	// 宿主尺寸/位置变化后重新铺满遮罩并重新居中。
	void syncOverlayToHost();

signals:
	void closed();

protected:
	void closeEvent(QCloseEvent* event) override;
	// 语言切换后：标题与关闭按钮提示要跟着换
	void changeEvent(QEvent* event) override;

	// 记下宿主主窗口（遮罩与居中定位用）。子类构造时调一次。
	void setPopupHost(QWidget* host) { m_popupHost = host; }

	// 交给宿主之前的准备：默认什么都不做。子类在这里异步取数据。
	// 由 openHosted() 在 show() 之后调用 —— 理由见 PopupWindow.cpp 里那一段。
	virtual void refreshOnOpen() {}

private:
	void retranslateUi();

	QLabel* m_titleLabel = nullptr;
	QPushButton* m_closeButton = nullptr;
	QVBoxLayout* m_mainLayout = nullptr;
	QVBoxLayout* m_contentLayout = nullptr;
	// 记住标题：空标题时显示可翻译的默认名，切换语言要重算
	QString m_title;
	// 宿主主窗口：遮罩铺在它上面、弹窗相对它居中
	QWidget* m_popupHost = nullptr;
};
