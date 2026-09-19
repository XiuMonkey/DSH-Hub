#pragma once

// ------------------------------------------------------------------
// TopBarTools.h
// ------------------------------------------------------------------
// 顶栏工具的「功能半」（不含任何控件）：ToolsFilter。
//
// 它读写的是 DSH 里 ToolsFilterPlugin 插件暴露的 /api/tools-filter：
//   GET  /api/tools-filter?session=<会话 id>
//        → 该会话可见的工具目录（名字 + 描述 + 参数格式）+ 当前过滤状态
//   POST /api/tools-filter  { sessionId, config: { FilterList, DropGuidance } }
//        → 整份写回该会话的 ToolsFilterConfig.json
//
// 会话打开时调 ensureSession()：先拉目录；该会话还没有配置文件时，用**工具名**
// 建一份初始版：插件工具按插件名分目录，原版自带工具按功能分目录；描述与参数只
// 留在内存里给界面用，不写进 json。没有 `directory` 字段时才回退到 Default。
//
// 配置文件的形状（与 resources/ToolsFilterPlugin/example.ToolsFilterConfig.json 同族）：
//   { "FilterList": [
//       { "Directory": { "IsExpanded": "True", "DirectoryName": "Default",
//                        "ToolsList": [ "read", "glob",
//                                       { "ToolName": "pwsh", "IsVisible": "False" } ] } },
//       { "Directory": { "IsExpanded": "True", "DirectoryName": "built-in",
//                        "Description": "…", "ToolsList": [ … ] } } ] }
//   · 可见的工具就是名字（不写参数/描述）；被隐藏的那一项才写成 {ToolName, IsVisible:"False"}
//     —— 插件的判定是按 IsVisible，"从列表里删掉"表达不了隐藏；
//   · FilterList 里可以有任意多个目录。初始文档由插件的 `directory` 元数据生成
//     （插件工具按插件名，原版工具按功能）；之后界面按存储文档把工具分回各自的
//     目录，没被任何目录认领的工具全部进 Default。
//
// 过滤语义（与插件侧一致，见 resources/ToolsFilterPlugin/index.js）：隐藏只影响
// "发给模型的工具清单"（省 token），工具仍然可以被调用。
//
// 目录的 `IsExpanded` 是**筛选语义**（"False" = 整组隐藏，见插件的 compileFilterList），
// 与界面上的展开/收起无关 —— 界面上点目录名只是把该目录下的工具行显示出来，
// 所以这个值平时只做原样带回。唯一的例外：每个目录条目工具行底部的「折叠/可见」
// 开关会改写它所属目录的这个值（见 TopBar.cpp 的 groupHiddenChanged 信号）。
//
// 异步：全部走 QNetworkAccessManager 的完成回调，主线程只做收发、从不等待
// （没有 waitForFinished / 没有阻塞循环），因此不会卡界面。
//
// 为什么这个头文件里没有 Q_OBJECT：它在 vcxproj 里登记为普通 ClInclude（不跑 moc），
// 所以异步结果一律用 std::function 回调返回。UI 半在 include/TopBar.h（QtMoc ✔）。
// ------------------------------------------------------------------

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QNetworkAccessManager;
class QUrl;

// 一个工具：描述与参数只用于界面，不写进 json
struct ToolFilterEntry
{
	QString name;
	QString description;
	QString parameters;   // 参数 JSON Schema 的紧凑文本（可能为空）
	bool visible = true;  // 写进 json 的那一项
};

// 一个目录（FilterList 里的一项）：界面上一枚按钮 + 它名下的工具行
struct ToolFilterDirectory
{
	QString name;         // DirectoryName（界面上的按钮标题）
	QString description;  // Description（可空；界面只拿它做按钮提示）
	bool expanded = true; // 配置里的 IsExpanded（"False" = 整组隐藏），写回时原样带回
	QVector<ToolFilterEntry> tools;
};

// 一次 GET 的结果
struct ToolFilterCatalog
{
	bool ok = false;
	QString error;            // ok=false 时给人看的原因
	QString sessionId;
	bool degraded = false;    // 服务端只答出了全局层（目录≈空）
	bool configFound = false; // 该会话已经有过滤配置文件
	bool dropGuidance = true; // 顶层 DropGuidance（写回时原样带回）
	QStringList hideContexts; // 顶层 HideContexts（写回时原样带回，否则会被整份替换冲掉）
	int visibleCount = 0;     // 服务端算出的可见数（用于对账）
	QVector<ToolFilterDirectory> directories;
};

class ToolsFilter : public QObject
{
public:
	explicit ToolsFilter(QObject* parent = nullptr);

	// 基地址：只借它的 scheme/host/port（见 .cpp 里 endpointUrl 的说明）
	void setBaseUrl(const QUrl& url);

	// 会话打开时的入口：拉目录；没有配置文件就用工具名建一份初始版。
	// created=true 表示这次建了初始版。全程异步。
	void ensureSession(const QString& sessionId,
		std::function<void(bool ok, const ToolFilterCatalog& catalog, bool created, const QString& error)> done);
	// 只拉取（界面上的"刷新"）
	void fetch(const QString& sessionId, std::function<void(const ToolFilterCatalog&)> done);
	// 整份写回（勾选变化后）
	void save(const QString& sessionId, const QVector<ToolFilterDirectory>& directories, bool dropGuidance,
		const QStringList& hideContexts, std::function<void(bool ok, const QString& error)> done);

	// ------------------------------------------------------------------
	// 纯函数（无 IO，便于单测）
	// ------------------------------------------------------------------
	// 插件目录 + 已存的 FilterList → 界面用的目录表（目录按存储顺序，Default 一定在）
	static QVector<ToolFilterDirectory> buildDirectories(const QJsonArray& tools, const QJsonObject& storedConfig);
	// 一个目录的工具表 → ToolsList（可见的只写名字，隐藏的写 {ToolName, IsVisible:"False"}）
	static QJsonArray buildToolsList(const QVector<ToolFilterEntry>& tools);
	// 目录表 → 整份配置文件（save() 写的就是它；抽出来是为了可单测/可对账）
	static QJsonObject buildDocument(const QVector<ToolFilterDirectory>& directories, bool dropGuidance,
		const QStringList& hideContexts = QStringList());
	// 统计（状态行用）
	static int toolCount(const QVector<ToolFilterDirectory>& directories);
	static int hiddenCount(const QVector<ToolFilterDirectory>& directories);
	// 程序自动建立的目录名（Default）
	static QString defaultDirectoryName();

private:
	QUrl endpointUrl() const;

	QNetworkAccessManager* m_nam = nullptr;
	QUrl m_baseUrl;
};
