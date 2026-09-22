// ------------------------------------------------------------------
// DllCaller.cpp
// ------------------------------------------------------------------
// 实现 JSON5 DLL 描述解析和 DLL 调用。
//
// 支持的 JSON5 描述示例：
// {
//   "Name": "Example",
//   "Description": "Example DLL",
//   "Function": [
//     {
//       "Func": "dll_echo",
//       "InterfaceType": "default",      // 可选：default(http/websocket/com 等由未来版本接入)
//       "Calling Convention": {
//         "Style": "json",
//         "ReturnType": "int",
//         "ResultIsJson": true
//       },
//       "Tool": "dll_echo"
//     }
//   ]
// }
// ------------------------------------------------------------------

#include "ExtensionSystem/DllCaller.h"
#include "ExtensionSystem/Thunk.h"
#include "ExtensionSystem/ComCaller.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <cstdint>
#include <utility>

namespace
{
	// Windows 文件系统大小写不敏感：统一规范化绝对路径（存在时用 canonical），
	// 用于扩展描述去重与 DLL 库缓存的 key。
	QString normalizedDllPath(const QString& path)
	{
		const QFileInfo info(path);
		const QString canonical = info.canonicalFilePath();
		const QString absolute = canonical.isEmpty() ? info.absoluteFilePath() : canonical;
		return QDir::cleanPath(absolute);
	}
} // namespace

DllCaller::DllCaller() = default;

DllCaller::~DllCaller()
{
	unloadLibrary();
}

bool DllCaller::loadDescriptor(const QString& json5Path)
{
	QMutexLocker stateLock(&m_mutex);

	QFile file(json5Path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		m_errorString = QStringLiteral("cannot open descriptor: %1").arg(json5Path);
		return false;
	}

	const QByteArray raw = file.readAll();
	file.close();

	Descriptor parsed;
	QString error;
	if (!parseDescriptor(raw, &parsed, &error)) {
		m_errorString = error;
		return false;
	}

	// 扩展名 = 描述文件所在目录的 basename（安装布局 extensions/<name>/regulation.json5），
	// 与扩展管理/移除流程的扩展名同源。
	const QString dir = QFileInfo(json5Path).absolutePath();
	QString name = QFileInfo(dir).fileName();
	if (name.isEmpty())
		name = QFileInfo(json5Path).baseName();

	// 同名扩展已加载则拒绝添加（先移除再重装；重复扫描时靠此去重）
	for (const LoadedExtension& ext : m_extensions) {
		if (ext.name.compare(name, Qt::CaseInsensitive) == 0) {
			m_errorString = QStringLiteral("extension already loaded: %1 (remove it first)").arg(name);
			return false;
		}
	}

	// 每个 Function 的 LoadingSource 相对本描述文件目录解析成规范化绝对路径，
	// 调用期一律按绝对路径加载 DLL（多扩展同名 main.dll 互不冲突）
	for (FunctionSpec& fn : parsed.functions)
		fn.resolvedDllPath = normalizedDllPath(QDir(dir).filePath(fn.loadingSource));

	// 同名工具直接拒绝（与“重复注册即失败”的服务端语义一致）
	QSet<QString> occupied;
	for (const LoadedExtension& ext : m_extensions) {
		for (const FunctionSpec& fn : ext.descriptor.functions) {
			if (!fn.tool.isEmpty())
				occupied.insert(fn.tool);
		}
	}
	QSet<QString> selfSeen;
	for (const FunctionSpec& fn : parsed.functions) {
		if (fn.tool.isEmpty())
			continue;
		if (selfSeen.contains(fn.tool)) {
			m_errorString = QStringLiteral("descriptor %1 registers tool \"%2\" more than once").arg(name, fn.tool);
			return false;
		}
		selfSeen.insert(fn.tool);
		if (occupied.contains(fn.tool)) {
			m_errorString = QStringLiteral("tool \"%1\" already registered by another extension").arg(fn.tool);
			return false;
		}
	}

	LoadedExtension entry;
	entry.name = name;
	entry.descriptor = std::move(parsed);
	m_extensions.append(std::move(entry));

	qInfo().noquote() << QStringLiteral("[DllCaller] descriptor loaded: %1 name=%2 functions=%3")
		.arg(json5Path, name)
		.arg(m_extensions.last().descriptor.functions.size());
	return true;
}

bool DllCaller::loadLibrary(const QString& dllPath)
{
	return libraryForPath(dllPath) != nullptr;
}

void DllCaller::unloadLibrary()
{
	QMutexLocker stateLock(&m_mutex);

	for (auto it = m_librariesByPath.begin(); it != m_librariesByPath.end(); ++it) {
		LoadedLibrary* entry = it.value();
		if (!entry)
			continue;
		// 等待该 DLL 上的在途调用结束，避免卸载中的 QLibrary 被使用
		while (entry->inFlight > 0)
			entry->drained.wait(&m_mutex);
		if (entry->library) {
			entry->library->unload();
			delete entry->library;
		}
		delete entry;
	}
	m_librariesByPath.clear();
	m_extensions.clear();
	m_errorString.clear();
}

bool DllCaller::removeExtension(const QString& name)
{
	QMutexLocker stateLock(&m_mutex);

	int index = -1;
	for (int i = 0; i < m_extensions.size(); ++i) {
		if (m_extensions.at(i).name.compare(name, Qt::CaseInsensitive) == 0) {
			index = i;
			break;
		}
	}

	if (index < 0) {
		m_errorString = QStringLiteral("extension not loaded: %1").arg(name);
		return false;
	}

	// 先移除条目，让之后到达的调用查不到该扩展的工具
	m_extensions.removeAt(index);

	// 其余扩展仍在使用的 DLL 需要保留
	QSet<QString> keep;
	for (const LoadedExtension& ext : m_extensions) {
		for (const FunctionSpec& fn : ext.descriptor.functions) {
			if (!fn.resolvedDllPath.isEmpty())
				keep.insert(fn.resolvedDllPath);
		}
	}

	// 卸载仅属于被移除扩展的 DLL：等待其上的在途调用结束，释放文件占用
	// （Windows 下删除/覆盖才能成功）
	QStringList drop;
	for (auto it = m_librariesByPath.constBegin(); it != m_librariesByPath.constEnd(); ++it) {
		if (!keep.contains(it.key()))
			drop.append(it.key());
	}
	for (const QString& key : drop) {
		LoadedLibrary* entry = m_librariesByPath.take(key);
		if (!entry)
			continue;
		while (entry->inFlight > 0)
			entry->drained.wait(&m_mutex);
		if (entry->library) {
			entry->library->unload();
			delete entry->library;
		}
		delete entry;
	}

	qInfo().noquote() << QStringLiteral("[DllCaller] extension removed: %1").arg(name);
	return true;
}

QLibrary* DllCaller::libraryForPath(const QString& absoluteDllPath)
{
	QMutexLocker stateLock(&m_mutex);
	LoadedLibrary* entry = ensureRuntimeLocked(absoluteDllPath);
	return entry ? entry->library : nullptr;
}

// 假定调用方已持有 m_mutex：按规范化绝对路径查找，不存在则加载并登记
DllCaller::LoadedLibrary* DllCaller::ensureRuntimeLocked(const QString& absoluteDllPath)
{
	const QString key = normalizedDllPath(absoluteDllPath);
	auto it = m_librariesByPath.constFind(key);
	if (it != m_librariesByPath.constEnd())
		return it.value();

	if (key.isEmpty()) {
		m_errorString = QStringLiteral("empty DLL path");
		return nullptr;
	}

	if (!QFile::exists(key)) {
		m_errorString = QStringLiteral("cannot find DLL: %1").arg(key);
		return nullptr;
	}

	auto* entry = new LoadedLibrary;
	entry->library = new QLibrary(key);
	entry->library->setLoadHints(QLibrary::ResolveAllSymbolsHint);
	if (!entry->library->load()) {
		m_errorString = QStringLiteral("cannot load DLL: %1 (%2)")
			.arg(key, entry->library->errorString());
		delete entry->library;
		delete entry;
		return nullptr;
	}

	// 新约定：扩展 DLL 若导出 ClearMem，说明它的 json 风格工具返回的是
	// malloc 堆内存，调用方必须在该函数返回后调用 ClearMem 归还。
	// 老扩展（无 ClearMem 导出）保持旧行为（static 缓冲、不释放）。
	entry->clearMem = reinterpret_cast<ClearMemFn>(entry->library->resolve("ClearMem"));

	m_librariesByPath.insert(key, entry);
	qInfo().noquote() << "[DllCaller] library loaded:" << key;
	return entry;
}

void DllCaller::recordError(QString* out, const QString& message)
{
	QMutexLocker stateLock(&m_mutex);
	m_errorString = message;
	if (out)
		*out = message;
}

void DllCaller::releaseResult(const FunctionSpec& fn, const char* raw)
{
	if (!raw)
		return;
	QMutexLocker stateLock(&m_mutex);
	auto it = m_librariesByPath.constFind(normalizedDllPath(fn.resolvedDllPath));
	if (it == m_librariesByPath.constEnd() || !it.value() || !it.value()->clearMem)
		return; // 该 DLL 未导出 ClearMem（旧约定），不释放
	it.value()->clearMem(raw);
}

QStringList DllCaller::tools() const
{
	QMutexLocker stateLock(&m_mutex);

	QStringList result;
	for (const LoadedExtension& ext : m_extensions) {
		for (const FunctionSpec& fn : ext.descriptor.functions) {
			if (!fn.tool.isEmpty())
				result.append(fn.tool);
		}
	}
	return result;
}

QString DllCaller::errorString() const
{
	QMutexLocker stateLock(&m_mutex);
	return m_errorString;
}

bool DllCaller::callTool(const QString& tool,
	const QJsonObject& args,
	QJsonObject& result,
	QString* errorMessage)
{
	// 内部一律走 out 参数传递错误（并发安全）；m_errorString 仅在需要时由 recordError 维护
	QString fallbackError;
	QString* errOut = errorMessage ? errorMessage : &fallbackError;

	// 快照工具描述：只拷贝值，不在锁内持有指向容器内部元素的指针
	FunctionSpec spec;
	bool found = false;
	{
		QMutexLocker stateLock(&m_mutex);
		for (const LoadedExtension& ext : m_extensions) {
			for (const FunctionSpec& fn : ext.descriptor.functions) {
				if (fn.tool == tool) {
					spec = fn;
					found = true;
					break;
				}
			}
			if (found)
				break;
		}
	}

	if (!found) {
		qWarning().noquote() << "[DllCaller] unknown tool:" << tool;
		recordError(errOut, QStringLiteral("unknown tool: %1").arg(tool));
		return false;
	}

	// 按接口类型分派执行。以后新增接口类型时，在下面加一个对应的
	// "else if (interfaceType == ...)" 分支即可，每个分支自带完整执行逻辑，
	// 无需改动其它分支；未声明的类型落到末尾 else 统一报错。
	const QString interfaceType = spec.interfaceType.isEmpty()
		? QStringLiteral("default")
		: spec.interfaceType;

	if (interfaceType == QStringLiteral("default")) {
		// ---- 默认接口：原生 DLL（json / native 调用约定） ----
		// 线程模型：本函数可在 Worker 线程调用。同一 DLL 经 runMutex 串行
		// （旧扩展可能带 static 缓冲 / 非重入代码），不同 DLL 并行执行；
		// 卸载/移除扩展会等待 inFlight 归零，不会卸载使用中的 QLibrary。
		LoadedLibrary* runtime = nullptr;
		{
			QMutexLocker stateLock(&m_mutex);
			runtime = ensureRuntimeLocked(spec.resolvedDllPath);
			if (runtime)
				++runtime->inFlight;
		}

		if (!runtime) {
			qWarning().noquote() << "[DllCaller] DLL not loaded, tool=" << tool
				<< " source=" << spec.loadingSource;
			recordError(errOut, QStringLiteral("DLL not loaded: %1").arg(spec.loadingSource));
			return false;
		}

		QMutexLocker runLock(&runtime->runMutex);
		bool invoked = false;
		if (spec.style == QStringLiteral("json")) {
			invoked = invokeJsonFunction(spec, args, result, errOut);
		}
		else if (spec.style == QStringLiteral("native")) {
			invoked = invokeNativeFunction(spec, args, result, errOut);
		}
		else {
			qWarning().noquote() << "[DllCaller] unsupported calling style:" << spec.style;
			recordError(errOut, QStringLiteral("unsupported calling style: %1").arg(spec.style));
		}

		{
			QMutexLocker stateLock(&m_mutex);
			if (--runtime->inFlight == 0)
				runtime->drained.wakeAll();
		}

		if (!invoked) {
			qWarning().noquote() << "[DllCaller] tool call failed tool=" << tool
				<< " error=" << *errOut;
			return false;
		}

		qInfo().noquote() << "[DllCaller] tool call succeeded tool=" << tool;
		return true;
	}
	else if (interfaceType == QStringLiteral("http")) {
		// ---- 预留：HTTP 接口（未来在此实现，如按 url/method 发起请求） ----
		qWarning().noquote() << "[DllCaller] interfaceType \"http\" not implemented yet, tool=" << tool;
		recordError(errOut, QStringLiteral("interfaceType \"http\" not implemented yet"));
		return false;
	}
	else if (interfaceType == QStringLiteral("websocket")) {
		// ---- 预留：WebSocket 接口（未来在此实现） ----
		qWarning().noquote() << "[DllCaller] interfaceType \"websocket\" not implemented yet, tool=" << tool;
		recordError(errOut, QStringLiteral("interfaceType \"websocket\" not implemented yet"));
		return false;
	}
	else if (interfaceType == QStringLiteral("com")) {
		// ---- COM 接口：由 ComCaller 执行（组件白名单在 "Com" 段声明）。
		// 每次调用新建组件实例、无共享状态，可跨线程并行（ComCaller 内部按线程初始化 COM）。
		if (!spec.comConfig.isEmpty()) {
			QString comError;
			if (!comcall::invoke(spec.comConfig, args, result, comError)) {
				recordError(errOut, comError);
				return false;
			}
			qInfo().noquote() << "[DllCaller] tool call succeeded tool=" << tool;
			return true;
		}

		qWarning().noquote() << "[DllCaller] com tool lacks Com config, tool=" << tool;
		recordError(errOut, qtTrId("com_missing_description"));
		return false;
	}

	qWarning().noquote() << "[DllCaller] unsupported interfaceType, tool=" << tool
		<< " interfaceType=" << interfaceType;
	recordError(errOut, QStringLiteral("unsupported interfaceType: %1 (supported: \"default\")")
		.arg(interfaceType));
	return false;
}

bool DllCaller::parseDescriptor(const QByteArray& json5,
	Descriptor* out,
	QString* error)
{
	const QByteArray cleaned = removeTrailingCommas(stripJson5Comments(json5));

	QJsonParseError parseError;
	const QJsonDocument doc = QJsonDocument::fromJson(cleaned, &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
		*error = QStringLiteral("JSON5 parse error: %1 at offset %2")
			.arg(parseError.errorString())
			.arg(parseError.offset);
		return false;
	}

	const QJsonObject root = doc.object();

	const QJsonArray functions = root.value(QStringLiteral("Function")).toArray();
	if (functions.isEmpty()) {
		*error = QStringLiteral("descriptor has no Function array");
		return false;
	}

	out->functions.clear();
	for (const QJsonValue& value : functions) {
		const QJsonObject obj = value.toObject();
		FunctionSpec fn;
		fn.function = obj.value(QStringLiteral("Func")).toString();
		fn.tool = obj.value(QStringLiteral("Tool")).toString();
		fn.loadingSource = obj.value(QStringLiteral("LoadingSource"))
			.toString(QStringLiteral("main.dll"));
		// InterfaceType：缺省 "default"（现有 DLL json/native 方式）；
		// "com" 由 ComCaller 接入；http / websocket 等为未来预留。
		fn.interfaceType = obj.value(QStringLiteral("InterfaceType"))
			.toString(QStringLiteral("default"));

		// com 等非 default 接口的可选配置段，例如：
		//   "Com": { "ProgId": "WScript.Shell" }
		// ProgId 在此固定（白名单式），运行时 args 不能任意指定组件。
		if (fn.interfaceType != QStringLiteral("default"))
			fn.comConfig = obj.value(QStringLiteral("Com")).toObject();

		const QJsonObject cc = obj.value(QStringLiteral("Calling Convention")).toObject();
		fn.style = cc.value(QStringLiteral("Style")).toString(QStringLiteral("json"));
		fn.returnType = cc.value(QStringLiteral("ReturnType")).toString(QStringLiteral("int"));
		fn.resultIsJson = cc.value(QStringLiteral("ResultIsJson")).toBool(true);

		const QJsonArray params = cc.value(QStringLiteral("Parameters"))
			.toArray(cc.value(QStringLiteral("Params")).toArray());
		for (const QJsonValue& paramValue : params) {
			const QJsonObject paramObj = paramValue.toObject();
			ParameterSpec param;
			param.name = paramObj.value(QStringLiteral("Name"))
				.toString(paramObj.value(QStringLiteral("name")).toString());
			param.type = paramObj.value(QStringLiteral("Type"))
				.toString(paramObj.value(QStringLiteral("type")).toString());
			param.from = paramObj.value(QStringLiteral("From"))
				.toString(paramObj.value(QStringLiteral("from")).toString());
			param.defaultValue = paramObj.value(QStringLiteral("Default"));
			fn.parameters.append(param);
		}

		// Tool 是所有接口类型都必需的；Func 仅 default（DLL）接口需要，
		// com 等接口没有 DLL 导出函数名。
		if (fn.tool.isEmpty()
			|| (fn.interfaceType == QStringLiteral("default") && fn.function.isEmpty())) {
			*error = qtTrId("com_function_requires_tool");
			return false;
		}

		out->functions.append(fn);
	}

	return true;
}

bool DllCaller::invokeJsonFunction(const FunctionSpec& fn,
	const QJsonObject& args,
	QJsonObject& result,
	QString* error)
{
	if (fn.style != QStringLiteral("json")) {
		recordError(error, QStringLiteral("unsupported calling style: %1 (only \"json\" is implemented)").arg(fn.style));
		return false;
	}

	const QByteArray argsJson = QJsonDocument(args).toJson(QJsonDocument::Compact);

	// 方式一：const char* Func(const char* argsJson)
	if (fn.returnType == QStringLiteral("string")) {
		using StringFn = const char* (*)(const char*);
		auto* symbol = reinterpret_cast<StringFn>(libraryForPath(fn.resolvedDllPath)->resolve(fn.function.toUtf8().constData()));
		if (!symbol) {
			recordError(error, QStringLiteral("cannot resolve function: %1 (%2)")
				.arg(fn.function, libraryForPath(fn.resolvedDllPath)->errorString()));
			return false;
		}

		const char* rawResult = symbol(argsJson.constData());
		if (!rawResult) {
			recordError(error, QStringLiteral("DLL function returned null: %1").arg(fn.function));
			return false;
		}

		// 内存契约（新约定）：若该 DLL 导出了 ClearMem，则 rawResult 指向
		// DLL 分配的堆内存。先把内容拷进 Qt 自己的 QByteArray（后续解析只
		// 用副本，不再碰 rawResult），然后立刻调用 ClearMem 归还 DLL 内存。
		// 老扩展 DLL 未导出 ClearMem 时 releaseResult 为空操作，
		// 保持其"static 缓冲、调用方无需释放"的旧行为。
		const QByteArray resultBytes(rawResult);
		releaseResult(fn, rawResult);

		if (fn.resultIsJson) {
			QJsonParseError parseError;
			const QJsonDocument resultDoc = QJsonDocument::fromJson(resultBytes, &parseError);
			if (parseError.error != QJsonParseError::NoError || !resultDoc.isObject()) {
				recordError(error, QStringLiteral("DLL result is not a JSON object: %1").arg(fn.function));
				return false;
			}
			result = resultDoc.object();
		}
		else {
			result.insert(QStringLiteral("value"), QString::fromUtf8(resultBytes));
		}
		return true;
	}

	// 方式二：int/void Func(const char* argsJson, char** resultJson)
	using IntFn = int (*)(const char*, char**);
	using VoidFn = void (*)(const char*, char**);

	char* resultPtr = nullptr;

	if (fn.returnType == QStringLiteral("void")) {
		auto* symbol = reinterpret_cast<VoidFn>(libraryForPath(fn.resolvedDllPath)->resolve(fn.function.toUtf8().constData()));
		if (!symbol) {
			recordError(error, QStringLiteral("cannot resolve function: %1 (%2)")
				.arg(fn.function, libraryForPath(fn.resolvedDllPath)->errorString()));
			return false;
		}
		symbol(argsJson.constData(), &resultPtr);
	}
	else {
		auto* symbol = reinterpret_cast<IntFn>(libraryForPath(fn.resolvedDllPath)->resolve(fn.function.toUtf8().constData()));
		if (!symbol) {
			recordError(error, QStringLiteral("cannot resolve function: %1 (%2)")
				.arg(fn.function, libraryForPath(fn.resolvedDllPath)->errorString()));
			return false;
		}

		const int code = symbol(argsJson.constData(), &resultPtr);
		if (code != 0) {
			recordError(error, QStringLiteral("DLL function returned error code %1: %2").arg(code).arg(fn.function));
			return false;
		}
	}

	if (!resultPtr) {
		recordError(error, QStringLiteral("DLL function did not write a result: %1").arg(fn.function));
		return false;
	}

	const QByteArray resultBytes(resultPtr);
	releaseResult(fn, resultPtr);   // 拷走后立即归还 DLL 堆内存（若有 ClearMem 约定）

	if (fn.resultIsJson) {
		QJsonParseError parseError;
		const QJsonDocument resultDoc = QJsonDocument::fromJson(resultBytes, &parseError);
		if (parseError.error != QJsonParseError::NoError || !resultDoc.isObject()) {
			recordError(error, QStringLiteral("DLL result is not a JSON object: %1").arg(fn.function));
			return false;
		}
		result = resultDoc.object();
	}
	else {
		result.insert(QStringLiteral("value"), QString::fromUtf8(resultBytes));
	}

	return true;
}
bool DllCaller::invokeNativeFunction(const FunctionSpec& fn,
	const QJsonObject& args,
	QJsonObject& result,
	QString* error)
{
	Thunk::Signature signature;

	// 返回值类型
	if (fn.returnType == QStringLiteral("void"))
		signature.returnType = Thunk::ReturnType::Void;
	else if (fn.returnType == QStringLiteral("bool"))
		signature.returnType = Thunk::ReturnType::Bool;
	else if (fn.returnType == QStringLiteral("double"))
		signature.returnType = Thunk::ReturnType::Double;
	else if (fn.returnType == QStringLiteral("string")
		|| fn.returnType == QStringLiteral("const char*")
		|| fn.returnType == QStringLiteral("char*"))
		signature.returnType = Thunk::ReturnType::String;
	else
		signature.returnType = Thunk::ReturnType::Int;

	if (fn.parameters.size() > 16) {
		recordError(error, QStringLiteral("native caller currently supports at most 16 parameters"));
		return false;
	}

	QVector<Thunk::Arg> nativeArgs;
	nativeArgs.resize(fn.parameters.size());
	QVector<QByteArray> stringStorage;
	stringStorage.reserve(fn.parameters.size());

	for (int i = 0; i < fn.parameters.size(); ++i) {
		const ParameterSpec& param = fn.parameters.at(i);
		const QString& type = param.type;

		Thunk::ArgType argType;
		if (type == QStringLiteral("bool")) {
			argType = Thunk::ArgType::Bool;
		}
		else if (type == QStringLiteral("int")) {
			argType = Thunk::ArgType::Int;
		}
		else if (type == QStringLiteral("double")) {
			argType = Thunk::ArgType::Double;
		}
		else if (type == QStringLiteral("string") || type == QStringLiteral("const char*") || type == QStringLiteral("char*")) {
			argType = Thunk::ArgType::String;
		}
		else if (type == QStringLiteral("pointer32") || type == QStringLiteral("ptr32")) {
			argType = Thunk::ArgType::Pointer32;
		}
		else if (type == QStringLiteral("pointer64") || type == QStringLiteral("ptr64")) {
			argType = Thunk::ArgType::Pointer64;
		}
		else {
			recordError(error, QStringLiteral("unsupported native parameter type: %1").arg(type));
			return false;
		}

		signature.args.append(argType);

		const QString key = param.from.isEmpty() ? param.name : param.from;
		QJsonValue value = args.value(key);
		if (value.isUndefined() && !param.defaultValue.isUndefined())
			value = param.defaultValue;

		Thunk::Arg& native = nativeArgs[i];
		switch (argType) {
		case Thunk::ArgType::Bool:
		case Thunk::ArgType::Int:
			native.as.i = value.toInt();
			break;
		case Thunk::ArgType::Double:
			native.as.d = value.toDouble();
			break;
		case Thunk::ArgType::String:
		case Thunk::ArgType::Pointer64:
			stringStorage.append(value.toString().toUtf8());
			native.as.s = stringStorage.last().constData();
			break;
		case Thunk::ArgType::Pointer32:
			native.as.u32 = static_cast<std::uint32_t>(value.toDouble());
			break;
		}
	}

	auto* symbol = libraryForPath(fn.resolvedDllPath)->resolve(fn.function.toUtf8().constData());
	if (!symbol) {
		recordError(error, QStringLiteral("cannot resolve function: %1 (%2)")
			.arg(fn.function, libraryForPath(fn.resolvedDllPath)->errorString()));
		return false;
	}

	Thunk::Thunk thunk;
	if (!thunk.build(signature, symbol)) {
		recordError(error, thunk.errorString());
		return false;
	}

	switch (signature.returnType) {
	case Thunk::ReturnType::Void: {
		int dummy = 0;
		thunk.call(nativeArgs.constData(), &dummy);
		result.insert(QStringLiteral("ok"), true);
		break;
	}
	case Thunk::ReturnType::Bool: {
		bool value = false;
		thunk.call(nativeArgs.constData(), &value);
		result.insert(QStringLiteral("value"), value);
		break;
	}
	case Thunk::ReturnType::Int: {
		int value = 0;
		thunk.call(nativeArgs.constData(), &value);
		result.insert(QStringLiteral("value"), value);
		break;
	}
	case Thunk::ReturnType::Double: {
		double value = 0.0;
		thunk.call(nativeArgs.constData(), &value);
		result.insert(QStringLiteral("value"), value);
		break;
	}
	case Thunk::ReturnType::String: {
		const char* value = nullptr;
		thunk.call(nativeArgs.constData(), &value);
		result.insert(QStringLiteral("value"), QString::fromUtf8(value ? value : ""));
		break;
	}
	}

	return true;
}
QByteArray DllCaller::stripJson5Comments(const QByteArray& input)
{
	QByteArray output;
	output.reserve(input.size());

	bool inString = false;
	bool escape = false;
	int i = 0;
	const int n = input.size();

	while (i < n) {
		const char c = input.at(i);

		if (inString) {
			output.append(c);
			if (escape) {
				escape = false;
			}
			else if (c == '\\') {
				escape = true;
			}
			else if (c == '"') {
				inString = false;
			}
			++i;
			continue;
		}

		if (c == '"') {
			inString = true;
			output.append(c);
			++i;
			continue;
		}

		if (c == '/' && i + 1 < n && input.at(i + 1) == '/') {
			while (i < n && input.at(i) != '\n')
				++i;
			continue;
		}

		if (c == '/' && i + 1 < n && input.at(i + 1) == '*') {
			i += 2;
			while (i + 1 < n && !(input.at(i) == '*' && input.at(i + 1) == '/'))
				++i;
			i += 2;
			continue;
		}

		output.append(c);
		++i;
	}

	return output;
}

QByteArray DllCaller::removeTrailingCommas(const QByteArray& input)
{
	QByteArray output;
	output.reserve(input.size());

	bool inString = false;
	bool escape = false;
	int i = 0;
	const int n = input.size();

	while (i < n) {
		const char c = input.at(i);

		if (inString) {
			output.append(c);
			if (escape) {
				escape = false;
			}
			else if (c == '\\') {
				escape = true;
			}
			else if (c == '"') {
				inString = false;
			}
			++i;
			continue;
		}

		if (c == '"') {
			inString = true;
			output.append(c);
			++i;
			continue;
		}

		if (c == ',') {
			int j = i + 1;
			while (j < n && (input.at(j) == ' ' || input.at(j) == '\t' || input.at(j) == '\r' || input.at(j) == '\n'))
				++j;
			if (j < n && (input.at(j) == '}' || input.at(j) == ']')) {
				output.append(' ');
				i = j;
				continue;
			}
		}

		output.append(c);
		++i;
	}

	return output;
}