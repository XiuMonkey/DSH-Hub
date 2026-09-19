// ------------------------------------------------------------------
// ComCaller.cpp
// ------------------------------------------------------------------
// InterfaceType = "com" 的独立执行器实现：通过 IDispatch 自动化
// 调用白名单 ProgId 组件的方法/属性。命名不含 "Json"，与 DllCaller
// 的 json/native 风格是并列的执行路径。

#include "ComCaller.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonValue>
#include <QStringList>
#include <QVariant>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oaidl.h>
#include <oleauto.h>

#include <vector>

namespace
{
	// ------------------------------------------------------------------
	// COM 生命周期 / VARIANT 工具
	// ------------------------------------------------------------------

	// COM 必须按线程初始化。每次调用自行初始化（MTA）并在本调用结束时
	// 反初始化；若线程此前已被其它模式初始化（hr == S_FALSE 已初始化 /
	// RPC_E_CHANGED_MODE 已是 STA），则不再反初始化，避免破坏调用方线程
	// 的 COM 状态。这样 DLL/COM 工具可放到 Worker 线程并行执行。
	class ComThreadInit
	{
	public:
		ComThreadInit()
		{
			const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			m_ownsUninit = SUCCEEDED(hr) && hr != S_FALSE;
		}
		~ComThreadInit()
		{
			if (m_ownsUninit)
				CoUninitialize();
		}

	private:
		bool m_ownsUninit = false;
	};

	void clearVariant(VARIANT& v) { VariantClear(&v); }

	void clearVariants(std::vector<VARIANT>& list)
	{
		for (VARIANT& v : list)
			VariantClear(&v);
		list.clear();
	}

	VARIANT vtString(const QString& s)
	{
		VARIANT v = {};
		v.vt = VT_BSTR;
		v.bstrVal = SysAllocString(reinterpret_cast<const wchar_t*>(s.utf16()));
		return v;
	}

	VARIANT vtInt(long v)
	{
		VARIANT var = {};
		var.vt = VT_I4;
		var.lVal = v;
		return var;
	}

	VARIANT vtDouble(double v)
	{
		VARIANT var = {};
		var.vt = VT_R8;
		var.dblVal = v;
		return var;
	}

	VARIANT vtBool(bool v)
	{
		VARIANT var = {};
		var.vt = VT_BOOL;
		var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
		return var;
	}

	VARIANT vtMissing()
	{
		VARIANT var = {};
		var.vt = VT_ERROR;
		var.scode = DISP_E_PARAMNOTFOUND;
		return var;
	}

	// JSON 标量 -> VARIANT；数组/对象暂不支持（返回 false）。
	bool jsonToVariant(const QJsonValue& value, VARIANT& out)
	{
		if (value.isString()) {
			out = vtString(value.toString());
			return true;
		}
		if (value.isBool()) {
			out = vtBool(value.toBool());
			return true;
		}
		if (value.isDouble()) {
			const double d = value.toDouble();
			if (d == static_cast<long long>(d) && d >= -2147483648.0 && d <= 2147483647.0)
				out = vtInt(static_cast<long>(d));
			else
				out = vtDouble(d);
			return true;
		}
		if (value.isNull()) {
			VariantInit(&out);
			out.vt = VT_NULL;
			return true;
		}
		return false;
	}

	// VARIANT 标量 -> JSON。
	QJsonValue variantToJson(const VARIANT& v)
	{
		switch (v.vt) {
		case VT_BSTR:
			if (v.bstrVal)
				return QJsonValue(QString::fromWCharArray(v.bstrVal));
			return QJsonValue(QString());
		case VT_BOOL:
			return QJsonValue(v.boolVal != VARIANT_FALSE);
		case VT_I1: return QJsonValue((double)v.cVal);
		case VT_UI1: return QJsonValue((double)v.bVal);
		case VT_I2: return QJsonValue((double)v.iVal);
		case VT_UI2: return QJsonValue((double)v.uiVal);
		case VT_I4: case VT_INT: return QJsonValue((double)v.lVal);
		case VT_UI4: case VT_UINT: return QJsonValue((double)v.ulVal);
		case VT_I8: return QJsonValue((double)v.llVal);
		case VT_UI8: return QJsonValue((double)v.ullVal);
		case VT_R4: return QJsonValue((double)v.fltVal);
		case VT_R8: return QJsonValue(v.dblVal);
		case VT_NULL:
		case VT_EMPTY:
			return QJsonValue(QJsonValue::Null);
		default:
			return QJsonValue(QJsonValue::Null);
		}
	}

	// 用 VariantChangeType 尽力把任意 VARIANT 转成 VT_BSTR/VT_I4/VT_R8 后再序列化。
	QJsonValue coerceVariantToJson(const VARIANT& v)
	{
		VARIANT conv;
		VariantInit(&conv);
		QJsonValue result(QJsonValue::Null);

		if (SUCCEEDED(VariantChangeType(&conv, const_cast<VARIANT*>(&v), 0, VT_BSTR)) && conv.vt == VT_BSTR)
			result = QJsonValue(QString::fromWCharArray(conv.bstrVal));
		else if (SUCCEEDED(VariantChangeType(&conv, const_cast<VARIANT*>(&v), 0, VT_R8)) && conv.vt == VT_R8)
			result = QJsonValue(conv.dblVal);

		VariantClear(&conv);
		return result;
	}

	// ------------------------------------------------------------------
	// IDispatch 辅助（属性路径、方法/属性读写）
	// ------------------------------------------------------------------

	QString hrText(HRESULT hr)
	{
		return QStringLiteral("0x%1").arg((quint32)hr, 8, 16, QLatin1Char('0'));
	}

	DISPID resolveMember(IDispatch* obj, const QString& name, QString* error)
	{
		DISPID dispId = DISPID_UNKNOWN;
		const wchar_t* names[1] = {
			reinterpret_cast<const wchar_t*>(name.utf16())
		};
		HRESULT hr = obj->GetIDsOfNames(IID_NULL, const_cast<LPOLESTR*>(names), 1,
			LOCALE_USER_DEFAULT, &dispId);
		if (FAILED(hr) || dispId == DISPID_UNKNOWN) {
			if (error)
				*error = qtTrId("com_member_not_found_fmt").arg(name, hrText(hr));
			return DISPID_UNKNOWN;
		}
		return dispId;
	}

	// 读属性（PROPERTYGET），结果放入 out。
	bool getMemberValue(IDispatch* obj, const QString& name, VARIANT* out, QString* error)
	{
		DISPID dispId = resolveMember(obj, name, error);
		if (dispId == DISPID_UNKNOWN)
			return false;

		DISPPARAMS params = {};
		VARIANT result;
		VariantInit(&result);
		unsigned int argErr = 0;
		HRESULT hr = obj->Invoke(dispId, IID_NULL, LOCALE_USER_DEFAULT,
			DISPATCH_PROPERTYGET, &params, &result, nullptr, &argErr);
		if (FAILED(hr)) {
			if (error)
				*error = qtTrId("com_get_property_failed_fmt").arg(name, hrText(hr));
			return false;
		}
		if (out) {
			VariantInit(out);
			VariantCopy(out, &result);
		}
		VariantClear(&result);
		return true;
	}

	// 调用方法（METHOD），结果放入 out（可为空）。
	bool callMethod(IDispatch* obj, const QString& name,
		std::vector<VARIANT>& args, VARIANT* out, QString* error)
	{
		DISPID dispId = resolveMember(obj, name, error);
		if (dispId == DISPID_UNKNOWN)
			return false;

		// 参数需逆序填入 DISPPARAMS
		std::vector<VARIANT> reversed(args.rbegin(), args.rend());
		DISPPARAMS params = {};
		if (!reversed.empty()) {
			params.rgvarg = reversed.data();
			params.cArgs = (UINT)reversed.size();
		}

		VARIANT result;
		VariantInit(&result);
		unsigned int argErr = 0;
		HRESULT hr = obj->Invoke(dispId, IID_NULL, LOCALE_USER_DEFAULT,
			DISPATCH_METHOD, &params, &result, nullptr, &argErr);
		if (FAILED(hr)) {
			if (error) {
				QString reason = qtTrId("com_invoke_method_failed_fmt")
					.arg(name, hrText(hr));
				if (argErr < args.size())
					reason += qtTrId("com_arg_type_mismatch_fmt").arg(argErr);
				reason += QLatin1Char(')');
				*error = reason;
			}
			return false;
		}
		if (out) {
			VariantInit(out);
			VariantCopy(out, &result);
		}
		VariantClear(&result);
		return true;
	}

	// 写属性（PROPERTYPUT），值所有权转移给本函数。
	bool putMemberValue(IDispatch* obj, const QString& name, VARIANT value, QString* error)
	{
		DISPID dispId = resolveMember(obj, name, error);
		if (dispId == DISPID_UNKNOWN) {
			VariantClear(&value);
			return false;
		}

		DISPID putId = DISPID_PROPERTYPUT;
		DISPPARAMS params = {};
		params.rgvarg = &value;
		params.cArgs = 1;
		params.cNamedArgs = 1;
		params.rgdispidNamedArgs = &putId;

		unsigned int argErr = 0;
		HRESULT hr = obj->Invoke(dispId, IID_NULL, LOCALE_USER_DEFAULT,
			DISPATCH_PROPERTYPUT, &params, nullptr, nullptr, &argErr);
		VariantClear(&value);
		if (FAILED(hr)) {
			if (error)
				*error = qtTrId("com_put_property_failed_fmt").arg(name, hrText(hr));
			return false;
		}
		return true;
	}

	// 读整型属性（用于对象摘要，如 Count）。
	bool readIntProperty(IDispatch* obj, const QString& name, long& out)
	{
		VARIANT v;
		VariantInit(&v);
		if (!getMemberValue(obj, name, &v, nullptr))
			return false;
		bool ok = false;
		if (v.vt == VT_I4)
			out = v.lVal, ok = true;
		else if (v.vt == VT_I8)
			out = (long)v.llVal, ok = true;
		else {
			VARIANT conv;
			VariantInit(&conv);
			if (SUCCEEDED(VariantChangeType(&conv, &v, 0, VT_I4)) && conv.vt == VT_I4)
				out = conv.lVal, ok = true;
			VariantClear(&conv);
		}
		VariantClear(&v);
		return ok;
	}

	// 结果里若出现对象（VT_DISPATCH）：MVP 不支持句柄保活，
	// 只做最小"对象摘要"（尝试 Count），避免魔法探测过多。
	QJsonValue summarizeObject(IDispatch* p)
	{
		long count = 0;
		if (readIntProperty(p, QStringLiteral("Count"), count))
			return QJsonValue((double)count);
		return QJsonValue(QJsonValue::Null);
	}
} // namespace

namespace comcall
{
	bool invoke(const QJsonObject& comConfig,
		const QJsonObject& args,
		QJsonObject& result,
		QString& error)
	{
		ComThreadInit comInit;

		// 1. ProgId 白名单：只允许 regulation "Com" 段声明的组件
		const QString progId = comConfig.value(QStringLiteral("ProgId")).toString();
		if (progId.isEmpty()) {
			error = qtTrId("com_missing_config");
			return false;
		}

		// 2. 解析调用意图
		const QString member = args.value(QStringLiteral("member")).toString();
		if (member.isEmpty()) {
			error = qtTrId("com_missing_member_arg");
			return false;
		}
		QString kind = args.value(QStringLiteral("kind")).toString();
		if (kind.isEmpty())
			kind = QStringLiteral("method");
		if (kind != QStringLiteral("method") && kind != QStringLiteral("get") && kind != QStringLiteral("put")) {
			error = qtTrId("com_kind_unsupported");
			return false;
		}

		// 3. 创建组件实例（无状态：每次新建）
		CLSID clsid;
		HRESULT hr = CLSIDFromProgID(reinterpret_cast<const wchar_t*>(progId.utf16()), &clsid);
		if (FAILED(hr)) {
			error = qtTrId("com_progid_invalid_fmt").arg(progId, hrText(hr));
			return false;
		}
		IDispatch* root = nullptr;
		hr = CoCreateInstance(clsid, nullptr,
			CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER, IID_IDispatch, (void**)&root);
		if (FAILED(hr) || !root) {
			error = qtTrId("com_create_failed_fmt").arg(progId, hrText(hr));
			return false;
		}

		// 4. 沿属性链下行（path 每层都是 PROPERTYGET）
		IDispatch* current = root;
		QString currentOwner = progId;
		QStringList path;
		const QJsonValue pathValue = args.value(QStringLiteral("path"));
		if (pathValue.isArray()) {
			for (const QJsonValue& v : pathValue.toArray())
				path << v.toString();
		}

		for (const QString& prop : path) {
			VARIANT v;
			VariantInit(&v);
			QString stepError;
			if (!getMemberValue(current, prop, &v, &stepError)) {
				error = qtTrId("com_property_chain_failed_fmt")
					.arg(currentOwner, prop, stepError);
				root->Release();
				return false;
			}
			if (v.vt != VT_DISPATCH || !v.pdispVal) {
				error = qtTrId("com_property_not_object_fmt")
					.arg(currentOwner, prop);
				VariantClear(&v);
				root->Release();
				return false;
			}
			IDispatch* next = v.pdispVal;
			v.pdispVal = nullptr; // 让 VariantClear 不释放我们即将持有的指针
			VariantClear(&v);
			current = next;
			currentOwner += QStringLiteral(".") + prop;
		}

		// 5. 执行 member
		VARIANT out;
		VariantInit(&out);
		bool ok = false;

		if (kind == QStringLiteral("method")) {
			std::vector<VARIANT> argsList;
			const QJsonArray params = args.value(QStringLiteral("params")).toArray();
			for (const QJsonValue& pv : params) {
				VARIANT v;
				VariantInit(&v);
				if (!jsonToVariant(pv, v)) {
					error = qtTrId("com_params_type_unsupported_fmt")
						.arg(argsList.size());
					clearVariants(argsList);
					if (current != root) current->Release();
					root->Release();
					return false;
				}
				argsList.push_back(v);
			}
			QString mErr;
			ok = callMethod(current, member, argsList, &out, &mErr);
			clearVariants(argsList);
			if (!ok)
				error = mErr;
		}
		else if (kind == QStringLiteral("get")) {
			QString mErr;
			ok = getMemberValue(current, member, &out, &mErr);
			if (!ok)
				error = mErr;
		}
		else { // put
			const QJsonValue value = args.value(QStringLiteral("value"));
			VARIANT v;
			VariantInit(&v);
			if (!jsonToVariant(value, v)) {
				error = qtTrId("com_value_type_unsupported");
				if (current != root) current->Release();
				root->Release();
				return false;
			}
			QString mErr;
			ok = putMemberValue(current, member, v, &mErr);
			if (!ok)
				error = mErr;
		}

		// 6. 结果序列化
		if (ok) {
			if (out.vt == VT_DISPATCH && out.pdispVal) {
				result.insert(QStringLiteral("value"), summarizeObject(out.pdispVal));
				result.insert(QStringLiteral("object"), true);
				result.insert(QStringLiteral("note"),
					qtTrId("com_returned_object_summary_only"));
			}
			else {
				QJsonValue jv = variantToJson(out);
				if (jv.isNull() && out.vt != VT_NULL && out.vt != VT_EMPTY)
					jv = coerceVariantToJson(out); // 兜底转换
				result.insert(QStringLiteral("value"), jv);
			}
		}

		VariantClear(&out);
		if (current != root)
			current->Release();
		root->Release();
		return ok;
	}
} // namespace comcall