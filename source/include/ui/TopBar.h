#pragma once

// 对话顶部栏：左侧会话标题，右侧“工具过滤”入口（TopBar / ToolsFilterPopup / ToolsFilterDirectoryEntry）。
// 与界面无关的 HTTP / JSON / 配置形状都在 include/TopBarTools.h 的 ToolsFilter 里。目录行**默认全折叠**，
// 展开只是“这一次翻看”的显示状态；配置里的 IsExpanded（"False" = 整组隐藏）是另一回事。

#include "ui/StatusPopupWindow.h"

#include "VirtualClass/VirtualCommon.h"
#include "common/appearance/TopBarTools.h"

#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>
#include <functional>

class QAbstractButton;
class QCheckBox;
class QEvent;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QUrl;
class QVBoxLayout;

// 一个目录分组：表头按钮点击展开/收回名下工具行（纯显示、不写配置），
// 底部「折叠/可见」开关才写该目录的 IsExpanded（折叠 = 整组对模型隐藏）
class ToolsFilterDirectoryEntry : public QWidget
{
	Q_OBJECT

public:
	explicit ToolsFilterDirectoryEntry(const ToolFilterDirectory& directory, QWidget* parent = nullptr);

	bool isExpanded() const;
	void setExpanded(bool expanded);
	// 按当前的勾选状态刷新表头计数（“N 个工具 · 已隐藏 M 个”）
	void refreshMeta();

signals:
	// 表头被点：展开状态变了（只是显示，不写配置）
	void expandedChanged(const QString& directoryName, bool expanded);
	// 某个工具行的勾选变了（调用方据此回写配置）
	void toolVisibilityChanged(const QString& directoryName, const QString& toolName, bool visible);
	// collapsed 即勾选状态（勾上 = 折叠 = 整组隐藏）
	void groupHiddenChanged(const QString& directoryName, bool collapsed);

private:
	// 按整组隐藏状态同步开关文案与各行勾选框的可用性
	void applyGroupHiddenVisuals();

	QString m_name;
	QString m_description;
	bool m_groupHidden = false; // 配置里的 IsExpanded == "False"：插件层面整组隐藏
	// 展开状态必须自己记：窗口未显示时子控件的 isVisible() 一律为 false，
	// 用 m_body->isVisible() 反推会吞掉“第一次点击”
	bool m_expanded = true;
	QPushButton* m_header = nullptr;
	QLabel* m_chevron = nullptr;
	QLabel* m_nameLabel = nullptr;
	QLabel* m_metaLabel = nullptr;
	QWidget* m_body = nullptr;
	// 与 m_body 里的勾选框一一对应（下标即工具行号）
	QVector<QCheckBox*> m_boxes;
	QVector<QString> m_toolNames;
	// 写配置 IsExpanded，是本条目里唯一改筛选语义的控件；类型只用基类 ——
	// 头文件看不见 TopBar.cpp 匿名命名空间里的实现类
	QAbstractButton* m_groupToggle = nullptr;
};

// 工具过滤窗口：按目录分组的勾选列表，勾上 = 该工具会出现在发给模型的清单里；
// 隐藏只影响提示词（省 token），工具仍可被调用
class ToolsFilterPopup : public StatusPopupWindow
{
	Q_OBJECT

public:
	explicit ToolsFilterPopup(QWidget* parent = nullptr);

	// 打开前注入上下文（不持有 filter 所有权）；dropGuidance / hideContexts 保存时整份替换，必须原样带回
	void setContext(ToolsFilter* filter, const QString& sessionId, bool dropGuidance,
		const QStringList& hideContexts = QStringList());
	// 把一次 GET 的结果画上去（失败时显示原因）
	void applyCatalog(const ToolFilterCatalog& catalog);
	// 正在拉取时的状态
	void setBusy(bool busy);

signals:
	// “刷新”由 TopBar 去拉（弹窗自己不碰网络）
	void refreshRequested();

protected:
	// 语言切换后：说明文案、按钮、状态行跟着换
	void changeEvent(QEvent* event) override;

private:
	void retranslateStaticText();
	// 目录表 → 控件（重建全部目录行，展开状态按 m_collapsed 还原）
	void rebuild();
	void onDirectoryExpandedChanged(const QString& directoryName, bool expanded);
	void onToolVisibilityChanged(const QString& directoryName, const QString& toolName, bool visible);
	// 更新模型并写回配置（不重建，条目自己已更新显示）
	void onDirectoryGroupHiddenChanged(const QString& directoryName, bool collapsed);
	void setAllVisible(bool visible);
	void saveNow();
	// 当前目录表（含每行的勾选状态）
	QVector<ToolFilterDirectory> collectDirectories() const;
	void updateStatus();

	ToolsFilter* m_filter = nullptr; // 不持有所有权（TopBar 持有）
	QString m_sessionId;
	bool m_dropGuidance = true;
	QStringList m_hideContexts; // 原样带回（UI 保存是整份替换）
	bool m_updating = false; // 抑制 rebuild 期间的信号
	bool m_degraded = false; // 服务端只答出全局层时给个提示
	// 当前会话的目录表（描述/参数只在内存里）
	QVector<ToolFilterDirectory> m_directories;
	// 被收起的目录名，默认全折叠；判断“新会话”要用 m_collapsedSeedSession 而不是 m_sessionId
	// —— setContext 会先覆盖它，比较永远相等
	QSet<QString> m_collapsed;
	QString m_collapsedSeedSession;

	QLabel* m_hint = nullptr;
	QLabel* m_statusLabel = nullptr;
	QScrollArea* m_scroll = nullptr;
	QWidget* m_listContent = nullptr;
	QVBoxLayout* m_listLayout = nullptr; // 只装目录行（重建时整体清空）
	QPushButton* m_showAllButton = nullptr;
	QPushButton* m_hideAllButton = nullptr;
	QPushButton* m_refreshButton = nullptr;
};

class TopBar : public QWidget, public VirtualTopBar
{
	Q_OBJECT
	Q_INTERFACES(VirtualTopBar)

public:
	explicit TopBar(QWidget* parent = nullptr);
	// 析构时把自己从全局注册表摘掉（Destroy 带身份校验，见 CommonRegistry.h）
	~TopBar() override;

	void setTitle(const QString& title);
	// 会话切换时调用；换会话后会异步拉该会话的工具目录，没有配置文件时顺手建一份初始版
	void setSessionId(const QString& sessionId);
	// 服务端地址用回调现取（它会随启动/重启变化，回调省掉排序问题）
	void setBaseUrlProvider(std::function<QUrl()> provider);
	// 工具过滤入口的开关：后端被客户端扩展接管时关掉（那颗按钮走的是 DSH 服务端专属的
	// /api/tools-filter，自带 QNetworkAccessManager，与接管后的后端无关）
	void setToolsFilterEnabled(bool enabled);
	// 窗口缩放或移动后由 DSHHub 统一调：遮罩重新铺满 + 工具过滤窗口重新居中
	void syncOverlayToHost();

	// 交出顶栏横向布局（不转移所有权；调用方插进去的控件随后归顶栏所有）。跨 DLL 只能走
	// obj->qt_metacast(IID) 或接口虚函数 vtable：⚠️ 别改成 dynamic_cast（Itanium ABI 下跨模块静默
	// 返回 nullptr），也别 qobject_cast<TopBar*>（要 TopBar::staticMetaObject，宿主 exe 的外部符号
	// 且零导出 ⇒ 插件 DLL 链接期 LNK2019）
	QHBoxLayout* GetLayout() override;

protected:
	// 语言切换后：占位标题“未命名会话”要跟着换
	void changeEvent(QEvent* event) override;

private:
	void retranslateUi();
	void openToolsFilter();
	void closeToolsFilter();
	// 拉取该会话的工具目录；pushToPopup 表示顺便刷新打开的窗口
	void loadTools(bool pushToPopup);

	// **类级成员**（不能做成构造里的局部变量：外部要经 GetLayout() 拿它）；
	// 顺序：标题 | stretch | 已挂的外部控件 | 工具按钮
	QHBoxLayout* m_layout = nullptr;
	QLabel* m_titleLabel = nullptr;
	// 空标题时显示的是可翻译的占位文案，切换语言要重算
	QString m_title;

	QPushButton* m_toolsButton = nullptr;
	ToolsFilterPopup* m_toolsPopup = nullptr;
	ToolsFilter* m_filter = nullptr; // 功能半
	std::function<QUrl()> m_baseUrlProvider;
	QString m_sessionId;
	bool m_loading = false;
};
