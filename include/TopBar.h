#pragma once

// ------------------------------------------------------------------
// TopBar.h
// ------------------------------------------------------------------
// 对话顶部栏（左：会话标题；右：工具过滤入口）。
//   · TopBar            —— 白色圆角 + 细边框的那条栏，右侧多一个"工具过滤"按钮
//   · ToolsFilterPopup  —— 该按钮打开的窗口（继承 StatusPopupWindow = PopupWindow
//                          + 两行省略的状态栏）；灰色蒙版由 TopBar 负责铺（同
//                          PluginsManager / ExtensionManagerPopup 的做法）
//
// 与界面无关的功能（HTTP / JSON / 配置形状）都在 include/TopBarTools.h 的
// ToolsFilter 里，这里只管控件与交互。
// ------------------------------------------------------------------

#include "StatusPopupWindow.h"

#include "TopBarTools.h"

#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class QEvent;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QUrl;

// ------------------------------------------------------------------
// 工具过滤窗口
// ------------------------------------------------------------------
// 一个扁平的勾选列表：勾上 = 该工具会出现在发给模型的清单里，取消 = 隐藏。
// 隐藏只影响提示词（省 token），工具仍然可以被调用；描述与参数只用来做提示，
// 不写进配置文件。
class ToolsFilterPopup : public StatusPopupWindow
{
	Q_OBJECT

public:
	explicit ToolsFilterPopup(QWidget* parent = nullptr);

	// 打开前注入上下文：功能对象（不持有所有权）+ 会话 id + 该会话的
	// DropGuidance / HideContexts（保存时整份替换，必须原样带回）
	void setContext(ToolsFilter* filter, const QString& sessionId, bool dropGuidance,
		const QStringList& hideContexts = QStringList());
	// 把一次 GET 的结果画上去（失败时显示原因）
	void applyCatalog(const ToolFilterCatalog& catalog);
	// 正在拉取时的状态
	void setBusy(bool busy);

signals:
	// "刷新"按钮：由 TopBar 去重新拉取（弹窗自己不碰网络）
	void refreshRequested();

protected:
	// 语言切换后：说明文案、按钮、状态行跟着换
	void changeEvent(QEvent* event) override;

private:
	void retranslateStaticText();
	void onItemChanged(QListWidgetItem* item);
	void setAllVisible(bool visible);
	void saveNow();
	// 从列表控件回收当前勾选状态
	QVector<ToolFilterEntry> collectEntries() const;
	void updateStatus();
	void populate();

	ToolsFilter* m_filter = nullptr;   // 不持有所有权（TopBar 持有）
	QString m_sessionId;
	bool m_dropGuidance = true;
	QStringList m_hideContexts;        // 原样带回（UI 保存是整份替换）
	bool m_updating = false;           // 抑制 populate/setCheckState 引发的回写
	bool m_degraded = false;           // 服务端只答出全局层时给个提示
	QVector<ToolFilterEntry> m_tools;  // 当前会话的工具表（描述/参数只在内存里）

	QLabel* m_hint = nullptr;
	QLabel* m_statusLabel = nullptr;
	QListWidget* m_list = nullptr;
	QPushButton* m_showAllButton = nullptr;
	QPushButton* m_hideAllButton = nullptr;
	QPushButton* m_refreshButton = nullptr;
};

// ------------------------------------------------------------------
// 顶部栏
// ------------------------------------------------------------------
class TopBar : public QWidget
{
	Q_OBJECT

public:
	explicit TopBar(QWidget* parent = nullptr);

	void setTitle(const QString& title);
	// 会话切换时调用（DSHHub 在 setTitle 的同一处调用）：换会话后会异步拉一次
	// 该会话的工具目录，没有配置文件时顺手建一份初始版。
	void setSessionId(const QString& sessionId);
	// 服务端地址用回调现取（它会随启动/重启变化，回调省掉排序问题）
	void setBaseUrlProvider(std::function<QUrl()> provider);
	// 窗口缩放或移动后：遮罩（全窗口共用那一层）重新铺满 + 工具过滤窗口重新居中
	// （DSHHub 统一调）
	void syncOverlayToHost();

protected:
	// 语言切换后：占位标题“未命名会话”要跟着换
	void changeEvent(QEvent* event) override;

private:
	void retranslateUi();
	void openToolsFilter();
	void closeToolsFilter();
	// 拉取该会话的工具目录；pushToPopup 表示顺便刷新打开的窗口
	void loadTools(bool pushToPopup);

	QLabel* m_titleLabel = nullptr;
	// 记住会话标题：空标题时显示的是可翻译的占位文案，切换语言要重算
	QString m_title;

	QPushButton* m_toolsButton = nullptr;
	ToolsFilterPopup* m_toolsPopup = nullptr;
	ToolsFilter* m_filter = nullptr;        // 功能半
	std::function<QUrl()> m_baseUrlProvider;
	QString m_sessionId;
	bool m_loading = false;
};
