#include "TestThunk.h"

#include "Thunk.h"
#include "DllCaller.h"

#include <atomic>
#include <thread>

#include <QTest>
#include <QString>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdint>
#include <cstring>

namespace
{
	__declspec(noinline) int zero()
	{
		return 42;
	}

	__declspec(noinline) int twice(int a)
	{
		return a * 2;
	}

	__declspec(noinline) int add3(int a, int b, int c)
	{
		return a + b + c;
	}

	// 9 个整型参数：前 4 个走寄存器（rcx/rdx/r8/r9），其余 5 个走栈。
	__declspec(noinline) int add9(int a, int b, int c, int d, int e,
		int f, int g, int h, int i)
	{
		return a + b + c + d + e + f + g + h + i;
	}

	__declspec(noinline) double mix(int a, double b, const char* s)
	{
		return a + b + static_cast<double>(std::strlen(s));
	}

	__declspec(noinline) double doubleOnly(double a)
	{
		return a * 2.0;
	}

	__declspec(noinline) int doubleToInt(double a)
	{
		return static_cast<int>(a);
	}

	__declspec(noinline) double pi()
	{
		return 3.5;
	}

	__declspec(noinline) const char* echo(const char* s)
	{
		return s;
	}

	__declspec(noinline) void noop(int, double, const char*)
	{
	}

	__declspec(noinline) std::uintptr_t passPtr32(void* __ptr32 p)
	{
		return static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(p) & 0xFFFFFFFFULL);
	}

	__declspec(noinline) std::uintptr_t passPtr64(void* p)
	{
		return reinterpret_cast<std::uintptr_t>(p);
	}
} // namespace

void TestThunk::testNoArg()
{
	Thunk::Thunk thunk;
	Thunk::Signature signature;
	signature.returnType = Thunk::ReturnType::Int;

	QVERIFY2(thunk.build(signature, reinterpret_cast<void*>(&zero)),
		qPrintable(thunk.errorString()));

	Thunk::Arg args[1] = {};
	int result = 0;
	thunk.call(args, &result);
	QCOMPARE(result, 42);
}

void TestThunk::testOneInt()
{
	Thunk::Thunk thunk;
	Thunk::Signature signature;
	signature.returnType = Thunk::ReturnType::Int;
	signature.args = { Thunk::ArgType::Int };

	QVERIFY2(thunk.build(signature, reinterpret_cast<void*>(&twice)),
		qPrintable(thunk.errorString()));

	Thunk::Arg args[1] = {};
	args[0].as.i = 21;

	int result = 0;
	thunk.call(args, &result);
	QCOMPARE(result, 42);
}

void TestThunk::testAdd3()
{
	Thunk::Thunk thunk;
	Thunk::Signature signature;
	signature.returnType = Thunk::ReturnType::Int;
	signature.args = { Thunk::ArgType::Int, Thunk::ArgType::Int, Thunk::ArgType::Int };

	QVERIFY2(thunk.build(signature, reinterpret_cast<void*>(&add3)),
		qPrintable(thunk.errorString()));

	Thunk::Arg args[3] = {};
	args[0].as.i = 1;
	args[1].as.i = 2;
	args[2].as.i = 3;

	int result = 0;
	thunk.call(args, &result);
	QCOMPARE(result, 6);
}

void TestThunk::testMix()
{
	Thunk::Thunk thunk;
	Thunk::Signature signature;
	signature.returnType = Thunk::ReturnType::Double;
	signature.args = { Thunk::ArgType::Int, Thunk::ArgType::Double, Thunk::ArgType::String };

	QVERIFY2(thunk.build(signature, reinterpret_cast<void*>(&mix)),
		qPrintable(thunk.errorString()));

	Thunk::Arg args[3] = {};
	args[0].as.i = 2;
	args[1].as.d = 1.5;
	args[2].as.s = "abc";

	double result = 0.0;
	thunk.call(args, &result);
	QCOMPARE(result, 2.0 + 1.5 + 3.0);
}

void TestThunk::testDoubleOnly()
{
	Thunk::Thunk thunk;
	Thunk::Signature signature;
	signature.returnType = Thunk::ReturnType::Double;
	signature.args = { Thunk::ArgType::Double };

	QVERIFY2(thunk.build(signature, reinterpret_cast<void*>(&doubleOnly)),
		qPrintable(thunk.errorString()));

	Thunk::Arg args[1] = {};
	args[0].as.d = 2.5;

	double result = 0.0;
	thunk.call(args, &result);
	QCOMPARE(result, 5.0);
}

void TestThunk::testDoubleToInt()
{
	Thunk::Thunk thunk;
	Thunk::Signature signature;
	signature.returnType = Thunk::ReturnType::Int;
	signature.args = { Thunk::ArgType::Double };

	QVERIFY2(thunk.build(signature, reinterpret_cast<void*>(&doubleToInt)),
		qPrintable(thunk.errorString()));

	Thunk::Arg args[1] = {};
	args[0].as.d = 3.75;

	int result = 0;
	thunk.call(args, &result);
	QCOMPARE(result, 3);
}

void TestThunk::testDoubleReturnNoArg()
{
	Thunk::Thunk thunk;
	Thunk::Signature signature;
	signature.returnType = Thunk::ReturnType::Double;

	QVERIFY2(thunk.build(signature, reinterpret_cast<void*>(&pi)),
		qPrintable(thunk.errorString()));

	Thunk::Arg args[1] = {};
	double result = 0.0;
	thunk.call(args, &result);
	QCOMPARE(result, 3.5);
}

void TestThunk::testEcho()
{
	Thunk::Thunk thunk;
	Thunk::Signature signature;
	signature.returnType = Thunk::ReturnType::String;
	signature.args = { Thunk::ArgType::String };

	QVERIFY2(thunk.build(signature, reinterpret_cast<void*>(&echo)),
		qPrintable(thunk.errorString()));

	Thunk::Arg args[1] = {};
	args[0].as.s = "hello";

	const char* result = nullptr;
	thunk.call(args, &result);
	QCOMPARE(QString::fromUtf8(result), QStringLiteral("hello"));
}

void TestThunk::testPointer32()
{
	Thunk::Thunk thunk;
	Thunk::Signature signature;
	signature.returnType = Thunk::ReturnType::Int;
	signature.args = { Thunk::ArgType::Pointer32 };

	QVERIFY2(thunk.build(signature, reinterpret_cast<void*>(&passPtr32)),
		qPrintable(thunk.errorString()));

	Thunk::Arg args[1] = {};
	args[0].as.u32 = 0x12345678U;

	std::uintptr_t result = 0;
	thunk.call(args, &result);
	QCOMPARE(result, std::uintptr_t(0x12345678U));
}

void TestThunk::testPointer64()
{
	Thunk::Thunk thunk;
	Thunk::Signature signature;
	signature.returnType = Thunk::ReturnType::String; // 按 64 位指针宽度写回
	signature.args = { Thunk::ArgType::Pointer64 };

	QVERIFY2(thunk.build(signature, reinterpret_cast<void*>(&passPtr64)),
		qPrintable(thunk.errorString()));

	const std::uintptr_t fakePointer = std::uintptr_t(0x123456789ABCDEF0ULL);
	Thunk::Arg args[1] = {};
	args[0].as.s = reinterpret_cast<const char*>(fakePointer);

	std::uintptr_t result = 0;
	thunk.call(args, &result);
	QCOMPARE(result, fakePointer);
}

void TestThunk::testVoid()
{
	Thunk::Thunk thunk;
	Thunk::Signature signature;
	signature.returnType = Thunk::ReturnType::Void;
	signature.args = { Thunk::ArgType::Int, Thunk::ArgType::Double, Thunk::ArgType::String };

	QVERIFY2(thunk.build(signature, reinterpret_cast<void*>(&noop)),
		qPrintable(thunk.errorString()));

	Thunk::Arg args[3] = {};
	args[0].as.i = 7;
	args[1].as.d = 2.5;
	args[2].as.s = "test";

	int result = 0;
	thunk.call(args, &result); // void 返回不会写 result，仅验证不崩溃
	QVERIFY(true);
}

void TestThunk::testSignatureFromJson()
{
	QFile file(QStringLiteral("unit_test/test_signature.json"));
	QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
		qPrintable(file.errorString()));

	const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
	QVERIFY(doc.isObject());

	const QJsonObject root = doc.object();
	const QJsonObject signatureObj = root.value(QStringLiteral("signature")).toObject();

	Thunk::Signature signature;
	const QString returnType = signatureObj.value(QStringLiteral("returnType")).toString();
	if (returnType == QStringLiteral("void"))
		signature.returnType = Thunk::ReturnType::Void;
	else if (returnType == QStringLiteral("bool"))
		signature.returnType = Thunk::ReturnType::Bool;
	else if (returnType == QStringLiteral("double"))
		signature.returnType = Thunk::ReturnType::Double;
	else if (returnType == QStringLiteral("string"))
		signature.returnType = Thunk::ReturnType::String;
	else
		signature.returnType = Thunk::ReturnType::Int;

	const QJsonArray argTypes = signatureObj.value(QStringLiteral("arguments")).toArray();
	for (const auto& value : argTypes) {
		const QString type = value.toString();
		if (type == QStringLiteral("bool"))
			signature.args.append(Thunk::ArgType::Bool);
		else if (type == QStringLiteral("double"))
			signature.args.append(Thunk::ArgType::Double);
		else if (type == QStringLiteral("string"))
			signature.args.append(Thunk::ArgType::String);
		else if (type == QStringLiteral("pointer32"))
			signature.args.append(Thunk::ArgType::Pointer32);
		else if (type == QStringLiteral("pointer64"))
			signature.args.append(Thunk::ArgType::Pointer64);
		else
			signature.args.append(Thunk::ArgType::Int);
	}

	Thunk::Thunk thunk;
	QVERIFY2(thunk.build(signature, reinterpret_cast<void*>(&add3)),
		qPrintable(thunk.errorString()));

	const QJsonArray argsArray = root.value(QStringLiteral("args")).toArray();
	Thunk::Arg args[3] = {};
	for (int i = 0; i < argsArray.size() && i < 3; ++i)
		args[i].as.i = argsArray.at(i).toInt();

	int result = 0;
	thunk.call(args, &result);
	QCOMPARE(result, root.value(QStringLiteral("expected")).toInt());
}

void TestThunk::testStackArguments()
{
	// 第 5 个起的参数走栈传递（Thunk.cpp: stackArgCount = args > 4 ? args - 4 : 0）。
	// 这条路径以前没有用例覆盖，导致 testTooManyArguments() 的上限失效后无人发现。
	Thunk::Thunk thunk;
	Thunk::Signature signature;
	signature.returnType = Thunk::ReturnType::Int;
	for (int i = 0; i < 9; ++i)
		signature.args.append(Thunk::ArgType::Int);

	QVERIFY2(thunk.build(signature, reinterpret_cast<void*>(&add9)),
		qPrintable(thunk.errorString()));

	Thunk::Arg args[9] = {};
	for (int i = 0; i < 9; ++i)
		args[i].as.i = i + 1;

	int result = 0;
	thunk.call(args, &result);
	QCOMPARE(result, 45);
}

void TestThunk::testTooManyArguments()
{
	// 实现上限是 16 个参数（Thunk.cpp 的 "thunk currently supports at most 16 arguments"），
	// 所以“超限”必须用 17 个；9 个属于合法签名，由 testStackArguments() 覆盖。
	Thunk::Thunk thunk;
	Thunk::Signature signature;
	signature.returnType = Thunk::ReturnType::Int;
	for (int i = 0; i < 17; ++i)
		signature.args.append(Thunk::ArgType::Int);

	QVERIFY(!thunk.build(signature, reinterpret_cast<void*>(&add3)));
	QVERIFY(!thunk.errorString().isEmpty());
}

void TestThunk::testNativeExtensionViaDllCaller()
{
	const QString extDir = QStringLiteral("test extension/NativeThunkExt");
	const QString jsonPath = extDir + QStringLiteral("/regulation.json5");
	const QString dllPath = extDir + QStringLiteral("/main.dll");

	QVERIFY2(QFile::exists(jsonPath), qPrintable(jsonPath));
	QVERIFY2(QFile::exists(dllPath), qPrintable(dllPath));

	DllCaller caller;
	QVERIFY2(caller.loadDescriptor(jsonPath), qPrintable(caller.errorString()));
	QVERIFY2(caller.loadLibrary(dllPath), qPrintable(caller.errorString()));

	QVERIFY(caller.tools().contains(QStringLiteral("native_add3")));
	QVERIFY(caller.tools().contains(QStringLiteral("native_echo")));
	QVERIFY(caller.tools().contains(QStringLiteral("native_mix")));

	{
		QJsonObject args;
		args.insert(QStringLiteral("a"), 1);
		args.insert(QStringLiteral("b"), 2);
		args.insert(QStringLiteral("c"), 3);

		QJsonObject result;
		QString error;
		QVERIFY2(caller.callTool(QStringLiteral("native_add3"), args, result, &error),
			qPrintable(error));
		QCOMPARE(result.value(QStringLiteral("value")).toInt(), 6);
	}

	{
		QJsonObject args;
		args.insert(QStringLiteral("text"), QStringLiteral("hello"));

		QJsonObject result;
		QString error;
		QVERIFY2(caller.callTool(QStringLiteral("native_echo"), args, result, &error),
			qPrintable(error));
		QCOMPARE(result.value(QStringLiteral("value")).toString(), QStringLiteral("hello"));
	}

	{
		QJsonObject args;
		args.insert(QStringLiteral("a"), 2);
		args.insert(QStringLiteral("b"), 1.5);
		args.insert(QStringLiteral("s"), QStringLiteral("abc"));

		QJsonObject result;
		QString error;
		QVERIFY2(caller.callTool(QStringLiteral("native_mix"), args, result, &error),
			qPrintable(error));
		QCOMPARE(result.value(QStringLiteral("value")).toDouble(), 2.0 + 1.5 + 3.0);
	}
}

void TestThunk::testMultiExtensionDllCaller()
{
	const QString nativeDir = QStringLiteral("test extension/NativeThunkExt");
	const QString nativeJson = nativeDir + QStringLiteral("/regulation.json5");
	const QString nativeDll = nativeDir + QStringLiteral("/main.dll");

	// 第二个扩展取自 harness 的 launch-root：那是“已安装的 FFmpeg 示例扩展”，
	// 由安装流程/发行包生成，且被 .gitignore 忽略。机器上没装过时它不存在，
	// 此时跳过而不是报失败（否则这套单测在任何干净检出上都必然红一条）。
	const QString ffmpegJson = QStringLiteral("resources/server/launch-root/FFmpegExt/regulation.json5");
	if (!QFile::exists(ffmpegJson))
		QSKIP("需要先把 FFmpegExt 示例扩展安装到 resources/server/launch-root 才能跑本条用例");

	DllCaller caller;

	// 1) 加载第一个扩展（native），工具可用
	QVERIFY2(caller.loadDescriptor(nativeJson), qPrintable(caller.errorString()));
	QVERIFY2(caller.loadLibrary(nativeDll), qPrintable(caller.errorString()));
	QVERIFY(caller.tools().contains(QStringLiteral("native_add3")));

	// 2) 追加加载第二个扩展：工具是合并，而不是把第一个替换掉
	QVERIFY2(caller.loadDescriptor(ffmpegJson), qPrintable(caller.errorString()));
	QVERIFY(caller.tools().contains(QStringLiteral("native_add3")));
	QVERIFY(caller.tools().contains(QStringLiteral("ffmpeg_probe")));
	QVERIFY(caller.tools().contains(QStringLiteral("ffmpeg_convert")));

	// 3) 同名扩展重复加载被拒绝
	QVERIFY(!caller.loadDescriptor(nativeJson));
	QVERIFY(!caller.loadDescriptor(ffmpegJson));

	// 4) 移除 FFmpegExt 后只剩 native 工具，且 native 仍可正常调用
	QVERIFY2(caller.removeExtension(QStringLiteral("FFmpegExt")), qPrintable(caller.errorString()));
	QVERIFY(!caller.tools().contains(QStringLiteral("ffmpeg_probe")));
	QVERIFY(caller.tools().contains(QStringLiteral("native_add3")));

	{
		QJsonObject args;
		args.insert(QStringLiteral("a"), 1);
		args.insert(QStringLiteral("b"), 2);
		args.insert(QStringLiteral("c"), 3);

		QJsonObject result;
		QString error;
		QVERIFY2(caller.callTool(QStringLiteral("native_add3"), args, result, &error),
			qPrintable(error));
		QCOMPARE(result.value(QStringLiteral("value")).toInt(), 6);
	}

	// 5) 重复移除同一扩展返回 false；最后按名移除 native 扩展后工具清空
	QVERIFY(!caller.removeExtension(QStringLiteral("FFmpegExt")));
	QVERIFY2(caller.removeExtension(QStringLiteral("NativeThunkExt")), qPrintable(caller.errorString()));
	QVERIFY(caller.tools().isEmpty());
}

void TestThunk::testConcurrentDllCaller()
{
	const QString extDir = QStringLiteral("test extension/NativeThunkExt");
	const QString jsonPath = extDir + QStringLiteral("/regulation.json5");
	const QString dllPath = extDir + QStringLiteral("/main.dll");

	DllCaller caller;
	QVERIFY2(caller.loadDescriptor(jsonPath), qPrintable(caller.errorString()));
	QVERIFY2(caller.loadLibrary(dllPath), qPrintable(caller.errorString()));

	// 两个线程同时对同一个 DllCaller 发起 native 调用（同一 DLL，DllCaller
	// 内按 DLL 串行）；验证并发调用不崩溃、结果正确、错误通道无串扰。
	std::atomic<bool> failed{ false };
	std::atomic<int> okCount{ 0 };

	auto worker = [&caller, &failed, &okCount](int base) {
		for (int i = 0; i < 300; ++i) {
			QJsonObject args;
			args.insert(QStringLiteral("a"), base + i);
			args.insert(QStringLiteral("b"), 1);
			args.insert(QStringLiteral("c"), 0);

			QJsonObject result;
			QString error;
			if (!caller.callTool(QStringLiteral("native_add3"), args, result, &error)) {
				failed = true;
				break;
			}
			if (result.value(QStringLiteral("value")).toInt() != base + i + 1) {
				failed = true;
				break;
			}
			++okCount;
		}
		};

	std::thread t1(worker, 0);
	std::thread t2(worker, 1000);
	t1.join();
	t2.join();

	QVERIFY(!failed.load());
	QCOMPARE(okCount.load(), 600);
}