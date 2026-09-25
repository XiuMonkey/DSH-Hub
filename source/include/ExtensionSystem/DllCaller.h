#pragma once

// 读取 JSON5 格式的 DLL 描述文件（regulation.json5）并按描述调用扩展 DLL 的导出函数：json 风格
// （const char* 出参返回 JSON）与 native 风格（由 Thunk 按真实 C 签名调用）都由本类负责。
// json 风格桥接签名：int|void Func(const char* argsJson, char** resultJson)；string Func(const char*
// argsJson)（返回 const char*，内容是 JSON）。工具 args 先序列化成 JSON 字符串再传给 DLL。
// 结果内存契约：ReturnType=string 且 DLL 导出了 ClearMem 时，返回值是 DLL malloc 的堆内存，本类读取后
// 立即调 ClearMem 归还；无 ClearMem 的老扩展保持“拷走即用、不释放”。

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QLibrary>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWaitCondition>

class DllCaller
{
public:
	struct ParameterSpec
	{
		QString name;
		QString type;
		QString from;
		QJsonValue defaultValue;
	};

	struct FunctionSpec
	{
		QString function; // DLL 导出函数名（default 接口使用；com 等接口可留空）
		QString tool; // DSH 工具名
		QString loadingSource; // 函数从哪个 DLL 导出，例如 "main.dll" / "bin/foo.dll"
		QString interfaceType; // 接口类型："default"(DLL json/native)、"com"；预留 http/websocket 等
		QString style; // "json"（当前支持）
		QString returnType; // "int" / "void" / "string"
		bool resultIsJson = true;
		QVector<ParameterSpec> parameters;
		// InterfaceType != default 时的接口配置（如 com: { "ProgId": "..." }）
		QJsonObject comConfig;
		// loadDescriptor 解析后填充：LoadingSource 相对本描述文件所在目录解析出的规范化绝对
		// DLL 路径；调用期一律按此绝对路径查找/加载库，避免多扩展同名 main.dll 按短文件名缓存串库
		QString resolvedDllPath;
	};

	struct Descriptor
	{
		QVector<FunctionSpec> functions;
	};

	DllCaller();
	~DllCaller();

	// 读取并解析 JSON5 描述文件
	bool loadDescriptor(const QString& json5Path);
	// 加载 DLL 文件
	bool loadLibrary(const QString& dllPath);
	// 释放全部已加载 DLL 与扩展描述符（析构时使用）
	void unloadLibrary();
	// 卸载单个扩展（按扩展名，即描述文件所在目录名）并释放其专属 DLL 的文件占用；其它扩展不受影响。移除扩展前调用。
	bool removeExtension(const QString& name);

	QStringList tools() const;
	QString errorString() const;

	// 按 tool 名调用 DLL 工具；args 会转成 JSON 字符串传给 DLL。工具的 interfaceType 为 "com" 时
	// 委托给 ComCaller（comcall::invoke）
	bool callTool(const QString& tool, const QJsonObject& args, QJsonObject& result, QString* errorMessage = nullptr);

private:
	bool parseDescriptor(const QByteArray& json5, Descriptor* out, QString* error);
	bool invokeJsonFunction(const FunctionSpec& fn, const QJsonObject& args, QJsonObject& result,
		QString* error);
	bool invokeNativeFunction(const FunctionSpec& fn, const QJsonObject& args, QJsonObject& result,
		QString* error);

	// 扩展 DLL 若导出 ClearMem（堆分配约定），调用方读取完结果后必须调用它释放返回指针。
	using ClearMemFn = void (*)(const char*);

	// 一个已加载扩展 = 扩展名 + 一份描述符。扩展名取描述文件所在目录的 basename（安装布局
	// extensions/<name>/regulation.json5），与扩展管理/移除流程同源；同名扩展不允许重复加载
	// （先移除再重装）
	struct LoadedExtension
	{
		QString name;
		Descriptor descriptor;
	};

	// 一个已加载的 DLL；按规范化绝对路径缓存，多扩展同名 main.dll 互不冲突。线程模型：callTool 在
	// Worker 线程执行，同一 DLL 的调用经 runMutex 串行（旧扩展可能带 static 缓冲/非重入代码）、
	// 不同 DLL 之间并行；inFlight/drained 用于卸载/移除时等待在途调用结束，防止卸载中的 QLibrary 被使用。
	struct LoadedLibrary
	{
		QLibrary* library = nullptr;
		ClearMemFn clearMem = nullptr;
		QMutex runMutex; // 同库执行互斥（跨库并行）
		int inFlight = 0; // 执行中调用数（m_mutex 保护）
		QWaitCondition drained; // inFlight 归零通知（m_mutex 保护）
	};

	// 返回 fn 对应 DLL 的已加载 QLibrary；未加载则按 resolvedDllPath 加载。
	// 失败时返回 nullptr 并设置 m_errorString
	QLibrary* libraryForPath(const QString& absoluteDllPath);

	// 假定调用方已持有 m_mutex：按规范化绝对路径取运行时条目，缺失则加载。
	// 失败时返回 nullptr 并设置 m_errorString
	LoadedLibrary* ensureRuntimeLocked(const QString& absoluteDllPath);

	// 并发安全地记录错误：写 m_errorString（加锁）并回填 out（可为空）
	void recordError(QString* out, const QString& message);

	// 释放 fn 所对应 DLL 返回的结果指针（仅当该 DLL 导出了 ClearMem）。raw 为 nullptr 时安全跳过；
	// 老扩展未导出 ClearMem 时不做任何事，兼容其“static 缓冲、调用方无需释放”的旧约定
	void releaseResult(const FunctionSpec& fn, const char* raw);

	QByteArray stripJson5Comments(const QByteArray& input);
	QByteArray removeTrailingCommas(const QByteArray& input);

	QVector<LoadedExtension> m_extensions; // 已加载扩展（支持多扩展并存）
	QHash<QString, LoadedLibrary*> m_librariesByPath; // 规范化绝对路径 -> DLL
	QString m_errorString;
	mutable QMutex m_mutex; // 保护 m_extensions / m_librariesByPath / m_errorString
};
