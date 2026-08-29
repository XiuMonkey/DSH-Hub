// ------------------------------------------------------------------
// DllJsonCaller.cpp
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

#include "DllJsonCaller.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QDebug>
#include <utility>

DllJsonCaller::DllJsonCaller() = default;

DllJsonCaller::~DllJsonCaller()
{
	unloadLibrary();
}

bool DllJsonCaller::loadDescriptor(const QString& json5Path)
{
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

	m_descriptorDir = QFileInfo(json5Path).absolutePath();
	m_descriptor = std::move(parsed);
	qInfo().noquote() << "[DllJsonCaller] descriptor loaded:" << json5Path
		<< " functions=" << m_descriptor.functions.size();
	return true;
}

bool DllJsonCaller::loadLibrary(const QString& dllPath)
{
	if (m_library) {
		m_library->unload();
		delete m_library;
		m_library = nullptr;
	}

	m_dllPath = dllPath;
	m_library = new QLibrary(dllPath);
	m_library->setLoadHints(QLibrary::ResolveAllSymbolsHint);

	if (!m_library->load()) {
		m_errorString = QStringLiteral("cannot load DLL: %1 (%2)")
			.arg(dllPath, m_library->errorString());
		return false;
	}

	m_libraries.insert(QFileInfo(dllPath).fileName(), m_library);
	qInfo().noquote() << "[DllJsonCaller] library loaded:" << dllPath;
	return true;
}

void DllJsonCaller::unloadLibrary()
{
	for (QLibrary* lib : std::as_const(m_libraries)) {
		if (lib) {
			lib->unload();
			delete lib;
		}
	}
	m_libraries.clear();
	m_library = nullptr;
	m_dllPath.clear();
	m_descriptor.functions.clear();
	m_descriptor.name.clear();
	m_descriptor.description.clear();
	m_errorString.clear();
}

QLibrary* DllJsonCaller::libraryForSource(const QString& source)
{
	const QString key = source.isEmpty() ? QStringLiteral("main.dll") : source;

	auto it = m_libraries.constFind(key);
	if (it != m_libraries.constEnd())
		return it.value();

	// 相对路径基于 regulation.json5 所在目录解析
	QString dllPath = key;
	if (QDir::isRelativePath(dllPath))
		dllPath = m_descriptorDir + QStringLiteral("/") + dllPath;

	if (!QFile::exists(dllPath)) {
		m_errorString = QStringLiteral("cannot find DLL for LoadingSource: %1").arg(key);
		return nullptr;
	}

	auto* lib = new QLibrary(dllPath);
	lib->setLoadHints(QLibrary::ResolveAllSymbolsHint);
	if (!lib->load()) {
		m_errorString = QStringLiteral("cannot load DLL %1: %2")
			.arg(dllPath, lib->errorString());
		delete lib;
		return nullptr;
	}

	m_libraries.insert(key, lib);
	qInfo().noquote() << "[DllJsonCaller] additional library loaded:" << dllPath;
	return lib;
}

bool DllJsonCaller::isReady() const
{
	return m_library && m_library->isLoaded() && !m_descriptor.functions.isEmpty();
}

QStringList DllJsonCaller::tools() const
{
	QStringList result;
	for (const FunctionSpec& fn : m_descriptor.functions) {
		if (!fn.tool.isEmpty())
			result.append(fn.tool);
	}
	return result;
}

QString DllJsonCaller::errorString() const
{
	return m_errorString;
}

bool DllJsonCaller::callTool(const QString& tool,
	const QJsonObject& args,
	QJsonObject& result,
	QString* errorMessage)
{
	const FunctionSpec* found = nullptr;
	for (const FunctionSpec& fn : m_descriptor.functions) {
		if (fn.tool == tool) {
			found = &fn;
			break;
		}
	}

	if (!found) {
		qWarning().noquote() << "[DllJsonCaller] unknown tool:" << tool;
		m_errorString = QStringLiteral("unknown tool: %1").arg(tool);
		if (errorMessage)
			*errorMessage = m_errorString;
		return false;
	}

	QLibrary* lib = libraryForSource(found->loadingSource);
	if (!lib || !lib->isLoaded()) {
		qWarning().noquote() << "[DllJsonCaller] DLL not loaded, tool=" << tool
			<< " source=" << found->loadingSource;
		m_errorString = QStringLiteral("DLL not loaded: %1").arg(found->loadingSource);
		if (errorMessage)
			*errorMessage = m_errorString;
		return false;
	}

	bool invoked = false;
	if (found->style == QStringLiteral("json")) {
		invoked = invokeJsonFunction(*found, args, result, errorMessage);
	}
	else if (found->style == QStringLiteral("native")) {
		invoked = invokeNativeFunction(*found, args, result, errorMessage);
	}
	else {
		qWarning().noquote() << "[DllJsonCaller] unsupported calling style:" << found->style;
		m_errorString = QStringLiteral("unsupported calling style: %1").arg(found->style);
		if (errorMessage)
			*errorMessage = m_errorString;
		return false;
	}

	if (!invoked) {
		qWarning().noquote() << "[DllJsonCaller] tool call failed tool=" << tool << " error=" << m_errorString;
		if (errorMessage)
			*errorMessage = m_errorString;
		return false;
	}

	qInfo().noquote() << "[DllJsonCaller] tool call succeeded tool=" << tool;
	return true;
}

bool DllJsonCaller::parseDescriptor(const QByteArray& json5,
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
	out->name = root.value(QStringLiteral("Name")).toString();
	out->description = root.value(QStringLiteral("Description")).toString();

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

		if (fn.function.isEmpty() || fn.tool.isEmpty()) {
			*error = QStringLiteral("each Function entry needs 'Func' and 'Tool'");
			return false;
		}

		out->functions.append(fn);
	}

	return true;
}

bool DllJsonCaller::invokeJsonFunction(const FunctionSpec& fn,
	const QJsonObject& args,
	QJsonObject& result,
	QString* error)
{
	if (fn.style != QStringLiteral("json")) {
		m_errorString = QStringLiteral("unsupported calling style: %1 (only \"json\" is implemented)").arg(fn.style);
		if (error)
			*error = m_errorString;
		return false;
	}

	const QByteArray argsJson = QJsonDocument(args).toJson(QJsonDocument::Compact);

	// 方式一：const char* Func(const char* argsJson)
	if (fn.returnType == QStringLiteral("string")) {
		using StringFn = const char* (*)(const char*);
		auto* symbol = reinterpret_cast<StringFn>(libraryForSource(fn.loadingSource)->resolve(fn.function.toUtf8().constData()));
		if (!symbol) {
			m_errorString = QStringLiteral("cannot resolve function: %1 (%2)")
				.arg(fn.function, libraryForSource(fn.loadingSource)->errorString());
			if (error)
				*error = m_errorString;
			return false;
		}

		const char* rawResult = symbol(argsJson.constData());
		if (!rawResult) {
			m_errorString = QStringLiteral("DLL function returned null: %1").arg(fn.function);
			if (error)
				*error = m_errorString;
			return false;
		}

		const QByteArray resultBytes(rawResult);
		if (fn.resultIsJson) {
			QJsonParseError parseError;
			const QJsonDocument resultDoc = QJsonDocument::fromJson(resultBytes, &parseError);
			if (parseError.error != QJsonParseError::NoError || !resultDoc.isObject()) {
				m_errorString = QStringLiteral("DLL result is not a JSON object: %1").arg(fn.function);
				if (error)
					*error = m_errorString;
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
		auto* symbol = reinterpret_cast<VoidFn>(libraryForSource(fn.loadingSource)->resolve(fn.function.toUtf8().constData()));
		if (!symbol) {
			m_errorString = QStringLiteral("cannot resolve function: %1 (%2)")
				.arg(fn.function, libraryForSource(fn.loadingSource)->errorString());
			if (error)
				*error = m_errorString;
			return false;
		}
		symbol(argsJson.constData(), &resultPtr);
	}
	else {
		auto* symbol = reinterpret_cast<IntFn>(libraryForSource(fn.loadingSource)->resolve(fn.function.toUtf8().constData()));
		if (!symbol) {
			m_errorString = QStringLiteral("cannot resolve function: %1 (%2)")
				.arg(fn.function, libraryForSource(fn.loadingSource)->errorString());
			if (error)
				*error = m_errorString;
			return false;
		}

		const int code = symbol(argsJson.constData(), &resultPtr);
		if (code != 0) {
			m_errorString = QStringLiteral("DLL function returned error code %1: %2").arg(code).arg(fn.function);
			if (error)
				*error = m_errorString;
			return false;
		}
	}

	if (!resultPtr) {
		m_errorString = QStringLiteral("DLL function did not write a result: %1").arg(fn.function);
		if (error)
			*error = m_errorString;
		return false;
	}

	const QByteArray resultBytes(resultPtr);

	if (fn.resultIsJson) {
		QJsonParseError parseError;
		const QJsonDocument resultDoc = QJsonDocument::fromJson(resultBytes, &parseError);
		if (parseError.error != QJsonParseError::NoError || !resultDoc.isObject()) {
			m_errorString = QStringLiteral("DLL result is not a JSON object: %1").arg(fn.function);
			if (error)
				*error = m_errorString;
			return false;
		}
		result = resultDoc.object();
	}
	else {
		result.insert(QStringLiteral("value"), QString::fromUtf8(resultBytes));
	}

	return true;
}
bool DllJsonCaller::invokeNativeFunction(const FunctionSpec& fn,
	const QJsonObject& args,
	QJsonObject& result,
	QString* error)
{
	enum ArgKind {
		ArgBool,
		ArgInt,
		ArgDouble,
		ArgString
	};

	enum RetKind {
		RetVoid,
		RetBool,
		RetInt,
		RetDouble,
		RetString
	};

	auto parseRet = [](const QString& text, RetKind* kind) -> bool {
		if (text == QStringLiteral("void")) { *kind = RetVoid; return true; }
		if (text == QStringLiteral("bool")) { *kind = RetBool; return true; }
		if (text == QStringLiteral("int")) { *kind = RetInt; return true; }
		if (text == QStringLiteral("double")) { *kind = RetDouble; return true; }
		if (text == QStringLiteral("string") || text == QStringLiteral("const char*") || text == QStringLiteral("char*")) {
			*kind = RetString;
			return true;
		}
		return false;
		};

	auto parseArg = [](const QString& text, ArgKind* kind) -> bool {
		if (text == QStringLiteral("bool")) { *kind = ArgBool; return true; }
		if (text == QStringLiteral("int")) { *kind = ArgInt; return true; }
		if (text == QStringLiteral("double")) { *kind = ArgDouble; return true; }
		if (text == QStringLiteral("string") || text == QStringLiteral("const char*") || text == QStringLiteral("char*")) {
			*kind = ArgString;
			return true;
		}
		return false;
		};

	RetKind retKind;
	if (!parseRet(fn.returnType, &retKind)) {
		m_errorString = QStringLiteral("unsupported native return type: %1").arg(fn.returnType);
		if (error)
			*error = m_errorString;
		return false;
	}

	if (fn.parameters.size() > 2) {
		m_errorString = QStringLiteral("native caller currently supports at most 2 parameters");
		if (error)
			*error = m_errorString;
		return false;
	}

	QVector<int> intValues;
	QVector<double> doubleValues;
	QVector<QByteArray> stringValues;

	QVector<int> intIdx(fn.parameters.size(), -1);
	QVector<int> doubleIdx(fn.parameters.size(), -1);
	QVector<int> stringIdx(fn.parameters.size(), -1);
	QVector<ArgKind> argKinds;

	for (int i = 0; i < fn.parameters.size(); ++i) {
		const ParameterSpec& param = fn.parameters.at(i);
		ArgKind argKind;
		if (!parseArg(param.type, &argKind)) {
			m_errorString = QStringLiteral("unsupported native parameter type: %1").arg(param.type);
			if (error)
				*error = m_errorString;
			return false;
		}

		const QString key = param.from.isEmpty() ? param.name : param.from;
		QJsonValue value = args.value(key);
		if (value.isUndefined() && !param.defaultValue.isUndefined())
			value = param.defaultValue;

		argKinds.append(argKind);
		switch (argKind) {
		case ArgBool:
		case ArgInt: {
			const int v = value.toInt();
			intIdx[i] = intValues.size();
			intValues.append(v);
			break;
		}
		case ArgDouble: {
			const double v = value.toDouble();
			doubleIdx[i] = doubleValues.size();
			doubleValues.append(v);
			break;
		}
		case ArgString: {
			QByteArray s = value.toString().toUtf8();
			stringIdx[i] = stringValues.size();
			stringValues.append(s);
			break;
		}
		}
	}

	auto* symbol = libraryForSource(fn.loadingSource)->resolve(fn.function.toUtf8().constData());
	if (!symbol) {
		m_errorString = QStringLiteral("cannot resolve function: %1 (%2)")
			.arg(fn.function, libraryForSource(fn.loadingSource)->errorString());
		if (error)
			*error = m_errorString;
		return false;
	}

	auto setVoid = [&result]() {
		result.insert(QStringLiteral("ok"), true);
		};
	auto setBool = [&result](bool v) {
		result.insert(QStringLiteral("value"), v);
		};
	auto setInt = [&result](int v) {
		result.insert(QStringLiteral("value"), v);
		};
	auto setDouble = [&result](double v) {
		result.insert(QStringLiteral("value"), v);
		};
	auto setString = [&result](const char* v) {
		result.insert(QStringLiteral("value"), QString::fromUtf8(v ? v : ""));
		};

	const int argCount = argKinds.size();

	if (argCount == 0) {
		switch (retKind) {
		case RetVoid: {
			auto f = reinterpret_cast<void (*)()>(symbol);
			f();
			setVoid();
			return true;
		}
		case RetBool: {
			auto f = reinterpret_cast<bool (*)()>(symbol);
			setBool(f());
			return true;
		}
		case RetInt: {
			auto f = reinterpret_cast<int (*)()>(symbol);
			setInt(f());
			return true;
		}
		case RetDouble: {
			auto f = reinterpret_cast<double (*)()>(symbol);
			setDouble(f());
			return true;
		}
		case RetString: {
			auto f = reinterpret_cast<const char* (*)()>(symbol);
			setString(f());
			return true;
		}
		}
		return true;
	}

	if (argCount == 1) {
		switch (argKinds.at(0)) {
		case ArgBool:
		case ArgInt: {
			const int a = intValues.at(intIdx.at(0));
			switch (retKind) {
			case RetVoid: { auto f = reinterpret_cast<void (*)(int)>(symbol); f(a); setVoid(); return true; }
			case RetBool: { auto f = reinterpret_cast<bool (*)(int)>(symbol); setBool(f(a)); return true; }
			case RetInt: { auto f = reinterpret_cast<int (*)(int)>(symbol); setInt(f(a)); return true; }
			case RetDouble: { auto f = reinterpret_cast<double (*)(int)>(symbol); setDouble(f(a)); return true; }
			case RetString: { auto f = reinterpret_cast<const char* (*)(int)>(symbol); setString(f(a)); return true; }
			}
			break;
		}
		case ArgDouble: {
			const double a = doubleValues.at(doubleIdx.at(0));
			switch (retKind) {
			case RetVoid: { auto f = reinterpret_cast<void (*)(double)>(symbol); f(a); setVoid(); return true; }
			case RetBool: { auto f = reinterpret_cast<bool (*)(double)>(symbol); setBool(f(a)); return true; }
			case RetInt: { auto f = reinterpret_cast<int (*)(double)>(symbol); setInt(f(a)); return true; }
			case RetDouble: { auto f = reinterpret_cast<double (*)(double)>(symbol); setDouble(f(a)); return true; }
			case RetString: { auto f = reinterpret_cast<const char* (*)(double)>(symbol); setString(f(a)); return true; }
			}
			break;
		}
		case ArgString: {
			const char* a = stringValues.at(stringIdx.at(0)).constData();
			switch (retKind) {
			case RetVoid: { auto f = reinterpret_cast<void (*)(const char*)>(symbol); f(a); setVoid(); return true; }
			case RetBool: { auto f = reinterpret_cast<bool (*)(const char*)>(symbol); setBool(f(a)); return true; }
			case RetInt: { auto f = reinterpret_cast<int (*)(const char*)>(symbol); setInt(f(a)); return true; }
			case RetDouble: { auto f = reinterpret_cast<double (*)(const char*)>(symbol); setDouble(f(a)); return true; }
			case RetString: { auto f = reinterpret_cast<const char* (*)(const char*)>(symbol); setString(f(a)); return true; }
			}
			break;
		}
		}
		return true;
	}

	// 两个参数
	switch (argKinds.at(0)) {
	case ArgBool:
	case ArgInt: {
		const int a = intValues.at(intIdx.at(0));
		switch (argKinds.at(1)) {
		case ArgBool:
		case ArgInt: {
			const int b = intValues.at(intIdx.at(1));
			switch (retKind) {
			case RetVoid: { auto f = reinterpret_cast<void (*)(int, int)>(symbol); f(a, b); setVoid(); return true; }
			case RetBool: { auto f = reinterpret_cast<bool (*)(int, int)>(symbol); setBool(f(a, b)); return true; }
			case RetInt: { auto f = reinterpret_cast<int (*)(int, int)>(symbol); setInt(f(a, b)); return true; }
			case RetDouble: { auto f = reinterpret_cast<double (*)(int, int)>(symbol); setDouble(f(a, b)); return true; }
			case RetString: { auto f = reinterpret_cast<const char* (*)(int, int)>(symbol); setString(f(a, b)); return true; }
			}
			break;
		}
		case ArgDouble: {
			const double b = doubleValues.at(doubleIdx.at(1));
			switch (retKind) {
			case RetVoid: { auto f = reinterpret_cast<void (*)(int, double)>(symbol); f(a, b); setVoid(); return true; }
			case RetBool: { auto f = reinterpret_cast<bool (*)(int, double)>(symbol); setBool(f(a, b)); return true; }
			case RetInt: { auto f = reinterpret_cast<int (*)(int, double)>(symbol); setInt(f(a, b)); return true; }
			case RetDouble: { auto f = reinterpret_cast<double (*)(int, double)>(symbol); setDouble(f(a, b)); return true; }
			case RetString: { auto f = reinterpret_cast<const char* (*)(int, double)>(symbol); setString(f(a, b)); return true; }
			}
			break;
		}
		case ArgString: {
			const char* b = stringValues.at(stringIdx.at(1)).constData();
			switch (retKind) {
			case RetVoid: { auto f = reinterpret_cast<void (*)(int, const char*)>(symbol); f(a, b); setVoid(); return true; }
			case RetBool: { auto f = reinterpret_cast<bool (*)(int, const char*)>(symbol); setBool(f(a, b)); return true; }
			case RetInt: { auto f = reinterpret_cast<int (*)(int, const char*)>(symbol); setInt(f(a, b)); return true; }
			case RetDouble: { auto f = reinterpret_cast<double (*)(int, const char*)>(symbol); setDouble(f(a, b)); return true; }
			case RetString: { auto f = reinterpret_cast<const char* (*)(int, const char*)>(symbol); setString(f(a, b)); return true; }
			}
			break;
		}
		}
		break;
	}
	case ArgDouble: {
		const double a = doubleValues.at(doubleIdx.at(0));
		switch (argKinds.at(1)) {
		case ArgBool:
		case ArgInt: {
			const int b = intValues.at(intIdx.at(1));
			switch (retKind) {
			case RetVoid: { auto f = reinterpret_cast<void (*)(double, int)>(symbol); f(a, b); setVoid(); return true; }
			case RetBool: { auto f = reinterpret_cast<bool (*)(double, int)>(symbol); setBool(f(a, b)); return true; }
			case RetInt: { auto f = reinterpret_cast<int (*)(double, int)>(symbol); setInt(f(a, b)); return true; }
			case RetDouble: { auto f = reinterpret_cast<double (*)(double, int)>(symbol); setDouble(f(a, b)); return true; }
			case RetString: { auto f = reinterpret_cast<const char* (*)(double, int)>(symbol); setString(f(a, b)); return true; }
			}
			break;
		}
		case ArgDouble: {
			const double b = doubleValues.at(doubleIdx.at(1));
			switch (retKind) {
			case RetVoid: { auto f = reinterpret_cast<void (*)(double, double)>(symbol); f(a, b); setVoid(); return true; }
			case RetBool: { auto f = reinterpret_cast<bool (*)(double, double)>(symbol); setBool(f(a, b)); return true; }
			case RetInt: { auto f = reinterpret_cast<int (*)(double, double)>(symbol); setInt(f(a, b)); return true; }
			case RetDouble: { auto f = reinterpret_cast<double (*)(double, double)>(symbol); setDouble(f(a, b)); return true; }
			case RetString: { auto f = reinterpret_cast<const char* (*)(double, double)>(symbol); setString(f(a, b)); return true; }
			}
			break;
		}
		case ArgString: {
			const char* b = stringValues.at(stringIdx.at(1)).constData();
			switch (retKind) {
			case RetVoid: { auto f = reinterpret_cast<void (*)(double, const char*)>(symbol); f(a, b); setVoid(); return true; }
			case RetBool: { auto f = reinterpret_cast<bool (*)(double, const char*)>(symbol); setBool(f(a, b)); return true; }
			case RetInt: { auto f = reinterpret_cast<int (*)(double, const char*)>(symbol); setInt(f(a, b)); return true; }
			case RetDouble: { auto f = reinterpret_cast<double (*)(double, const char*)>(symbol); setDouble(f(a, b)); return true; }
			case RetString: { auto f = reinterpret_cast<const char* (*)(double, const char*)>(symbol); setString(f(a, b)); return true; }
			}
			break;
		}
		}
		break;
	}
	case ArgString: {
		const char* a = stringValues.at(stringIdx.at(0)).constData();
		switch (argKinds.at(1)) {
		case ArgBool:
		case ArgInt: {
			const int b = intValues.at(intIdx.at(1));
			switch (retKind) {
			case RetVoid: { auto f = reinterpret_cast<void (*)(const char*, int)>(symbol); f(a, b); setVoid(); return true; }
			case RetBool: { auto f = reinterpret_cast<bool (*)(const char*, int)>(symbol); setBool(f(a, b)); return true; }
			case RetInt: { auto f = reinterpret_cast<int (*)(const char*, int)>(symbol); setInt(f(a, b)); return true; }
			case RetDouble: { auto f = reinterpret_cast<double (*)(const char*, int)>(symbol); setDouble(f(a, b)); return true; }
			case RetString: { auto f = reinterpret_cast<const char* (*)(const char*, int)>(symbol); setString(f(a, b)); return true; }
			}
			break;
		}
		case ArgDouble: {
			const double b = doubleValues.at(doubleIdx.at(1));
			switch (retKind) {
			case RetVoid: { auto f = reinterpret_cast<void (*)(const char*, double)>(symbol); f(a, b); setVoid(); return true; }
			case RetBool: { auto f = reinterpret_cast<bool (*)(const char*, double)>(symbol); setBool(f(a, b)); return true; }
			case RetInt: { auto f = reinterpret_cast<int (*)(const char*, double)>(symbol); setInt(f(a, b)); return true; }
			case RetDouble: { auto f = reinterpret_cast<double (*)(const char*, double)>(symbol); setDouble(f(a, b)); return true; }
			case RetString: { auto f = reinterpret_cast<const char* (*)(const char*, double)>(symbol); setString(f(a, b)); return true; }
			}
			break;
		}
		case ArgString: {
			const char* b = stringValues.at(stringIdx.at(1)).constData();
			switch (retKind) {
			case RetVoid: { auto f = reinterpret_cast<void (*)(const char*, const char*)>(symbol); f(a, b); setVoid(); return true; }
			case RetBool: { auto f = reinterpret_cast<bool (*)(const char*, const char*)>(symbol); setBool(f(a, b)); return true; }
			case RetInt: { auto f = reinterpret_cast<int (*)(const char*, const char*)>(symbol); setInt(f(a, b)); return true; }
			case RetDouble: { auto f = reinterpret_cast<double (*)(const char*, const char*)>(symbol); setDouble(f(a, b)); return true; }
			case RetString: { auto f = reinterpret_cast<const char* (*)(const char*, const char*)>(symbol); setString(f(a, b)); return true; }
			}
			break;
		}
		}
		break;
	}
	}

	m_errorString = QStringLiteral("unsupported native call combination");
	if (error)
		*error = m_errorString;
	return false;
}

QByteArray DllJsonCaller::stripJson5Comments(const QByteArray& input)
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

QByteArray DllJsonCaller::removeTrailingCommas(const QByteArray& input)
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