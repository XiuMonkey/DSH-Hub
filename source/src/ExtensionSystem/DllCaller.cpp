// JSON5 DLL 描述解析 + DLL 调用（json / native 两种风格；com 走 ComCaller）。

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
	QString normalizedDllPath(const QString& path)
	{
		const QFileInfo info(path);
		const QString canonical = info.canonicalFilePath();
		const QString absolute = canonical.isEmpty() ? info.absoluteFilePath() : canonical;
		return QDir::cleanPath(absolute);
	}
}

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

	// 扩展名 = 描述文件所在目录的 basename
	const QString dir = QFileInfo(json5Path).absolutePath();
	QString name = QFileInfo(dir).fileName();
	if (name.isEmpty())
		name = QFileInfo(json5Path).baseName();

	// 同名扩展已加载则拒绝添加
	for (const LoadedExtension& ext : m_extensions) {
		if (ext.name.compare(name, Qt::CaseInsensitive) == 0) {
			m_errorString = QStringLiteral("extension already loaded: %1 (remove it first)").arg(name);
			return false;
		}
	}

	// LoadingSource 解析成绝对路径，多扩展同名 main.dll 互不冲突
	for (FunctionSpec& fn : parsed.functions)
		fn.resolvedDllPath = normalizedDllPath(QDir(dir).filePath(fn.loadingSource));

	// 同名工具直接拒绝
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
		.arg(json5Path, name).arg(m_extensions.last().descriptor.functions.size());
	return true;
}

bool DllCaller::loadLibrary(const QString& dllPath)
{
	return libraryForPath(dllPath) != nullptr;
}

void DllCaller::unloadLibrary()
{
	QMutexLocker stateLock(&m_mutex);

	// 先清描述符再等：否则 delete 前可能又冒出新的调用
	m_extensions.clear();

	// 先收集路径再逐个等：wait 会放开 m_mutex
	const QStringList paths = m_librariesByPath.keys();
	for (const QString& key : paths) {
		LoadedLibrary* entry = m_librariesByPath.value(key);
		if (!entry)
			continue;
		while (entry->inFlight > 0)
			entry->drained.wait(&m_mutex);
		if (m_librariesByPath.value(key) != entry)
			continue;
		m_librariesByPath.remove(key);
		if (entry->library) {
			entry->library->unload();
			delete entry->library;
		}
		delete entry;
	}
	// 不在这里 clear()：被 continue 跳过的条目会丢指针
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

	m_extensions.removeAt(index);

	QSet<QString> keep;
	for (const LoadedExtension& ext : m_extensions) {
		for (const FunctionSpec& fn : ext.descriptor.functions) {
			if (!fn.resolvedDllPath.isEmpty())
				keep.insert(fn.resolvedDllPath);
		}
	}

	// 卸载仅属于该扩展的 DLL：等调用结束以释放文件占用
	QStringList drop;
	for (auto it = m_librariesByPath.constBegin(); it != m_librariesByPath.constEnd(); ++it) {
		if (!keep.contains(it.key()))
			drop.append(it.key());
	}
	for (const QString& key : drop) {
		// ⚠️ 顺序：先等在途调用归零、再摘掉条目；反过来在途调用的 libraryForPath() 查不到条目，
		// 重新加载同一 DLL 拿到新 runMutex，串行保证失效
		LoadedLibrary* entry = m_librariesByPath.value(key);
		if (!entry)
			continue;

		while (entry->inFlight > 0)
			entry->drained.wait(&m_mutex);

		// 等待期间 QHash 可能重哈希，重新确认条目没被换掉
		if (m_librariesByPath.value(key) != entry)
			continue;

		// inFlight 已归零且 runMutex 已释放，摘除安全
		m_librariesByPath.remove(key);
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

// 调用方须已持有 m_mutex
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
		m_errorString = QStringLiteral("cannot load DLL: %1 (%2)").arg(key, entry->library->errorString());
		delete entry->library;
		delete entry;
		return nullptr;
	}

	// 导出 ClearMem 的 DLL 返回 malloc 堆内存须归还；老扩展保持 static 缓冲
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
		return; // 未导出 ClearMem（旧约定），不释放
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

bool DllCaller::callTool(const QString& tool, const QJsonObject& args, QJsonObject& result, QString* errorMessage)
{
	// 走 out 参数传错误
	QString fallbackError;
	QString* errOut = errorMessage ? errorMessage : &fallbackError;

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

	const QString interfaceType = spec.interfaceType.isEmpty()
		? QStringLiteral("default")
		: spec.interfaceType;

	if (interfaceType == QStringLiteral("default")) {
		// 同一 DLL 经 runMutex 串行，不同 DLL 并行；卸载等 inFlight 归零
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

		bool invoked = false;
		{
			QMutexLocker runLock(&runtime->runMutex);
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
		}

		// ⚠️ 必须先释放 runMutex、再递减 inFlight：卸载方等到归零会连 runMutex 一起 delete 该
		// LoadedLibrary，否则 ~QMutexLocker 会 unlock 已 delete 的 QMutex
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
		qWarning().noquote() << "[DllCaller] interfaceType \"http\" not implemented yet, tool=" << tool;
		recordError(errOut, QStringLiteral("interfaceType \"http\" not implemented yet"));
		return false;
	}
	else if (interfaceType == QStringLiteral("websocket")) {
		qWarning().noquote() << "[DllCaller] interfaceType \"websocket\" not implemented yet, tool=" << tool;
		recordError(errOut, QStringLiteral("interfaceType \"websocket\" not implemented yet"));
		return false;
	}
	else if (interfaceType == QStringLiteral("com")) {
		// COM 由 ComCaller 执行（组件白名单在 "Com" 段声明）：每次新建实例、可跨线程并行
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
	recordError(errOut, QStringLiteral("unsupported interfaceType: %1 (supported: \"default\")").arg(interfaceType));
	return false;
}

bool DllCaller::parseDescriptor(const QByteArray& json5, Descriptor* out, QString* error)
{
	const QByteArray cleaned = removeTrailingCommas(stripJson5Comments(json5));

	QJsonParseError parseError;
	const QJsonDocument doc = QJsonDocument::fromJson(cleaned, &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
		*error = QStringLiteral("JSON5 parse error: %1 at offset %2").arg(parseError.errorString())
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
		fn.loadingSource = obj.value(QStringLiteral("LoadingSource")).toString(QStringLiteral("main.dll"));
		fn.interfaceType = obj.value(QStringLiteral("InterfaceType")).toString(QStringLiteral("default"));

		// 非 default 接口的配置段；ProgId 固定（白名单式）
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

		// Tool 对所有接口必需；Func 仅 default（DLL）接口需要
		if (fn.tool.isEmpty()
			|| (fn.interfaceType == QStringLiteral("default") && fn.function.isEmpty())) {
			*error = qtTrId("com_function_requires_tool");
			return false;
		}

		out->functions.append(fn);
	}

	return true;
}

bool DllCaller::invokeJsonFunction(const FunctionSpec& fn, const QJsonObject& args,
	QJsonObject& result, QString* error)
{
	if (fn.style != QStringLiteral("json")) {
		recordError(error, QStringLiteral("unsupported calling style: %1 (only \"json\" is implemented)")
			.arg(fn.style));
		return false;
	}

	const QByteArray argsJson = QJsonDocument(args).toJson(QJsonDocument::Compact);

	// libraryForPath() 在 DLL 缺失/加载失败时返回 nullptr
	QLibrary* library = libraryForPath(fn.resolvedDllPath);
	if (!library) {
		recordError(error, QStringLiteral("DLL not loaded for function %1: %2").arg(fn.function, errorString()));
		return false;
	}

	// 方式一：const char* Func(const char*)
	if (fn.returnType == QStringLiteral("string")) {
		using StringFn = const char* (*)(const char*);
		auto* symbol = reinterpret_cast<StringFn>(library->resolve(fn.function.toUtf8().constData()));
		if (!symbol) {
			recordError(error, QStringLiteral("cannot resolve function: %1 (%2)")
				.arg(fn.function, library->errorString()));
			return false;
		}

		const char* rawResult = symbol(argsJson.constData());
		if (!rawResult) {
			recordError(error, QStringLiteral("DLL function returned null: %1").arg(fn.function));
			return false;
		}

		// 内存契约：导出 ClearMem 时 rawResult 是堆内存，须先拷出再立刻归还
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

	// 方式二：int/void Func(const char*, char**)
	using IntFn = int (*)(const char*, char**);
	using VoidFn = void (*)(const char*, char**);

	char* resultPtr = nullptr;

	if (fn.returnType == QStringLiteral("void")) {
		auto* symbol = reinterpret_cast<VoidFn>(library->resolve(fn.function.toUtf8().constData()));
		if (!symbol) {
			recordError(error, QStringLiteral("cannot resolve function: %1 (%2)")
				.arg(fn.function, library->errorString()));
			return false;
		}
		symbol(argsJson.constData(), &resultPtr);
	}
	else {
		auto* symbol = reinterpret_cast<IntFn>(library->resolve(fn.function.toUtf8().constData()));
		if (!symbol) {
			recordError(error, QStringLiteral("cannot resolve function: %1 (%2)")
				.arg(fn.function, library->errorString()));
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
	releaseResult(fn, resultPtr); // 拷走后立即归还堆内存

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

bool DllCaller::invokeNativeFunction(const FunctionSpec& fn, const QJsonObject& args,
	QJsonObject& result, QString* error)
{
	Thunk::Signature signature;

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
		else if (type == QStringLiteral("string") || type == QStringLiteral("const char*") ||
			type == QStringLiteral("char*")) {
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

	// 同 invokeJsonFunction：取一次句柄并判空
	QLibrary* library = libraryForPath(fn.resolvedDllPath);
	if (!library) {
		recordError(error, QStringLiteral("DLL not loaded for function %1: %2").arg(fn.function, errorString()));
		return false;
	}

	auto* symbol = library->resolve(fn.function.toUtf8().constData());
	if (!symbol) {
		recordError(error, QStringLiteral("cannot resolve function: %1 (%2)")
			.arg(fn.function, library->errorString()));
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
