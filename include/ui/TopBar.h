#pragma once

// ------------------------------------------------------------------
// TopBar.h
// ------------------------------------------------------------------
// 对话顶部栏（左：会话标题；右：工具过滤入口）。
//   · TopBar                    —— 白色圆角 + 细边框的那条栏，右侧多一个"工具过滤"按钮
//   · ToolsFilterPopup          —— 该按钮打开的窗口（继承 StatusPopupWindow = PopupWindow
//                                  + 两行省略的状态栏）；灰色蒙版由 TopBar 负责铺（同
//                                  PluginsManager / ExtensionManagerPopup 的做法）
//   · ToolsFilterDirectoryEntry —— 窗口里的一个目录：表头是一枚按钮，点开/收回该目录
//                                  名下的工具行（纯显示，不改筛选语义）
//
// 与界面无关的功能（HTTP / JSON / 配置形状）都在 include/TopBarTools.h 的
// ToolsFilter 里，这里只管控件与交互。
//
// 窗口的形状：每个目录一枚按钮（DirectoryName），点一下展开它名下的工具行、再点收回；
// 工具行仍是勾选框（勾上 = 会出现在发给模型的清单里）。目录**默认全折叠**，
// 展开状态是"这一次翻看"的状态；配置里的 IsExpanded（"False" = 整组隐藏）是另一回事。
// 每个目录的工具行底部另有一枚「折叠/可见」开关 —— 写的就是该目录自己的
// IsExpanded（筛选语义：折叠 = 整组对模型隐藏）。
// ------------------------------------------------------------------

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

// ------------------------------------------------------------------
// 窗口里的一个目录
// ------------------------------------------------------------------
// 表头（目录名 + 计数）整行可点的按钮：点一下展开/收回下面的工具行。
// 表头右侧的箭头 ▾ / ▸ 表示当前是展开还是收回。
// 工具行底部有一枚「折叠/可见」胶囊开关（左文案右滑块，与勾选框样式区分）：
// 写配置里该目录的 IsExpanded（筛选语义，折叠 = 整组对模型隐藏）—— 与表头
// 那个纯显示的展开/收起不同。
class ToolsFilterDirectoryEntry : public QWidget
{
	Q_OBJECT

public:
	explicit ToolsFilterDirectoryEntry(const ToolFilterDirectory& directory, QWidget* parent = nullptr);

	QString directoryName() const;
	bool isExpanded() const;
	void setExpanded(bool expanded);
	// 按当前的勾选状态刷新表头计数（"N 个工具 · 已隐藏 M 个"）
	void refreshMeta();

signals:
	// 表头被点：展开状态变了（只是显示，不写配置）
	void expandedChanged(const QString& directoryName, bool expanded);
	// 某个工具行的勾选变了（调用方据此回写配置）
	void toolVisibilityChanged(const QString& directoryName, const QString& toolName, bool visible);
	// 底部「折叠/可见」开关变了：collapsed 即勾选状态（勾上 = 折叠 = 整组隐藏）
	void groupHiddenChanged(const QString& directoryName, bool collapsed);

private:
	// 按整组隐藏状态同步开关文案与各行勾选框的可用性
	void applyGroupHiddenVisuals();

	QString m_name;
	QString m_description;
	bool m_groupHidden = false;   // 配置里的 IsExpanded == "False"：插件层面整组隐藏
	// 展开状态自己记着，不用 m_body->isVisible() 反推：窗口还没显示时（重建发生在
	// 打开之前那种情况）子控件的 isVisible() 一律是 false，反推会让"第一次点击"被吞掉
	bool m_expanded = true;
	QPushButton* m_header = nullptr;
	QLabel* m_chevron = nullptr;
	QLabel* m_nameLabel = nullptr;
	QLabel* m_metaLabel = nullptr;
	QWidget* m_body = nullptr;
	// 与 m_body 里的勾选框一一对应（下标即工具行号）
	QVector<QCheckBox*> m_boxes;
	QVector<QString> m_toolNames;
	// 工具行底部的「折叠/可见」开关：左文案右胶囊滑块的整行按钮（CapsuleSwitchRow，
	// 实现在 TopBar.cpp）——写配置 IsExpanded，是本条目里唯一改筛选语义的控件。
	// 类型只用基类：头文件看不见 TopBar.cpp 匿名命名空间里的实现类。
	QAbstractButton* m_groupToggle = nullptr;
};

// ------------------------------------------------------------------
// 工具过滤窗口
// ------------------------------------------------------------------
// 按目录分组的勾选列表：勾上 = 该工具会出现在发给模型的清单里，取消 = 隐藏。
// 隐藏只影响提示词（省 token），工具仍然可以被调用；描述与参数只用来做提示，
// 不写进配置文件。每个目录工具行底部的「折叠/可见」开关写该目录的 IsExpanded
//（折叠 = 整组对模型隐藏，见上）。底部的 Agent 开关写的是 Agent 目录的 IsExpanded（见文件头）。
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
	// 目录表 → 控件（重建全部目录行；展开状态按 m_collapsed 还原）
	void rebuild();
	void onDirectoryExpandedChanged(const QString& directoryName, bool expanded);
	void onToolVisibilityChanged(const QString& directoryName, const QString& toolName, bool visible);
	// 某目录底部的「折叠/可见」开关变了：更新模型并写回配置（不重建，条目自己已更新显示）
	void onDirectoryGroupHiddenChanged(const QString& directoryName, bool collapsed);
	void setAllVisible(bool visible);
	void saveNow();
	// 当前目录表（含每行的勾选状态）
	QVector<ToolFilterDirectory> collectDirectories() const;
	void updateStatus();

	ToolsFilter* m_filter = nullptr;   // 不持有所有权（TopBar 持有）
	QString m_sessionId;
	bool m_dropGuidance = true;
	QStringList m_hideContexts;        // 原样带回（UI 保存是整份替换）
	bool m_updating = false;           // 抑制 rebuild 期间的信号
	bool m_degraded = false;           // 服务端只答出全局层时给个提示
	// 当前会话的目录表（描述/参数只在内存里）
	QVector<ToolFilterDirectory> m_directories;
	// 被收起的目录名。**默认全折叠**（观感干净）：新会话播种时把全部目录名记进来，
	// 用户随后的展开/收起照常增删——刷新与重开窗口都保留本次的翻看状态。
	// 判断"新会话"用 m_collapsedSeedSession（上次播种的会话号），不能用
	// m_sessionId：setContext() 会先把它覆盖成新会话，比较永远相等。
	QSet<QString> m_collapsed;
	QString m_collapsedSeedSession;

	QLabel* m_hint = nullptr;
	QLabel* m_statusLabel = nullptr;
	QScrollArea* m_scroll = nullptr;
	QWidget* m_listContent = nullptr;
	QVBoxLayout* m_listLayout = nullptr;   // 只装目录行（重建时整体清空）
	QPushButton* m_showAllButton = nullptr;
	QPushButton* m_hideAllButton = nullptr;
	QPushButton* m_refreshButton = nullptr;
};

// ------------------------------------------------------------------
// 顶部栏
// ------------------------------------------------------------------
class TopBar : public QWidget, public VirtualTopBar
{
	Q_OBJECT
		Q_INTERFACES(VirtualTopBar)
public:
	explicit TopBar(QWidget* parent = nullptr);
	// 析构时把自己从全局注册表摘掉（Destroy 带身份校验，见 CommonRegistry.h）
	~TopBar() override;

	void setTitle(const QString& title);
	// 会话切换时调用（DSHHub 在 setTitle 的同一处调用）：换会话后会异步拉一次
	// 该会话的工具目录，没有配置文件时顺手建一份初始版。
	void setSessionId(const QString& sessionId);
	// 服务端地址用回调现取（它会随启动/重启变化，回调省掉排序问题）
	void setBaseUrlProvider(std::function<QUrl()> provider);
	// 窗口缩放或移动后：遮罩（全窗口共用那一层）重新铺满 + 工具过滤窗口重新居中
	// （DSHHub 统一调）
	void syncOverlayToHost();

	// VirtualTopBar 接口的实现：交出顶栏的横向布局（见 .cpp 说明）。
	// 布局本身不转移所有权；调用方插进去的控件随后归顶栏所有。
	//
	// 独立编译的插件 DLL 侧这样用（它只需 include VirtualClass/VirtualTopBar.h）：
	//     if (auto* bar = qobject_cast<VirtualTopBar*>(host))
	//         if (auto* lay = bar->GetLayout()) lay->addWidget(myWidget);
	// 转换走 obj->qt_metacast(IID)：跨边界传的是**字符串**，插件侧零宿主符号；
	// 而 GetLayout() 是接口虚函数，走 vtable，同样不产生外部符号。
	// ⚠️ 别改成 dynamic_cast（Itanium ABI 下跨模块静默返回 nullptr），
	//    也别 qobject_cast<TopBar*>（要 TopBar::staticMetaObject，宿主 exe 的
	//    外部符号且零导出 ⇒ 插件 DLL 链接期 LNK2019）。
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

	// 顶栏的横向布局。**类级成员**（原先是构造里的局部变量）：外部代码要通过
	// GetLayout() 拿到它，局部变量在构造结束后就够不着了。
	// 布局顺序：标题 | stretch | 已挂的外部控件 | 工具按钮。GetLayout 的调用方
	// 若想插在工具按钮左侧，自行 indexOf 定位（见 TopBar.cpp 的实现说明）。
	QHBoxLayout* m_layout = nullptr;
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
