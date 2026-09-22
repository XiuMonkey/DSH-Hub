#include "TestClientExtension.h"

#include "ExtensionSystem/ClientExtension.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTest>

#if defined(Q_OS_WIN)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

QString TestClientExtension::makeExtensionDir(const QString& name, bool withRegulation) const
{
	const QString dir = QDir(m_root).filePath(name);
	QDir().mkpath(dir);

	QFile dll(QDir(dir).filePath(QStringLiteral("main.dll")));
	if (dll.open(QIODevice::WriteOnly | QIODevice::Truncate))
		dll.write("not-a-real-dll");   // 内容无所谓：本类测的是文件系统语义
	dll.close();

	if (withRegulation) {
		QFile reg(QDir(dir).filePath(QStringLiteral("regulation.json5")));
		if (reg.open(QIODevice::WriteOnly | QIODevice::Truncate))
			reg.write("{ \"Name\": \"" + name.toUtf8() + "\", \"Type\": \"ClientExtension\" }");
		reg.close();
	}

	return dir;
}

bool TestClientExtension::lockMainDll(const QString& dir)
{
#if defined(Q_OS_WIN)
	const QString path = QDir::toNativeSeparators(QDir(dir).filePath(QStringLiteral("main.dll")));
	// 只给 FILE_SHARE_READ：不给 FILE_SHARE_DELETE ⇒ 删除、以及父目录改名都会失败，
	// 与"dll 被本进程映射着"的表现一致（这条正是那个 bug 的根因）。
	HANDLE handle = ::CreateFileW(
		reinterpret_cast<const wchar_t*>(path.utf16()),
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (handle == INVALID_HANDLE_VALUE)
		return false;
	m_lockHandle = handle;
	return true;
#else
	Q_UNUSED(dir);
	return false;
#endif
}

void TestClientExtension::unlockMainDll()
{
#if defined(Q_OS_WIN)
	if (m_lockHandle) {
		::CloseHandle(static_cast<HANDLE>(m_lockHandle));
		m_lockHandle = nullptr;
	}
#endif
}

void TestClientExtension::initTestCase()
{
	// 指到临时目录：测试绝不能碰真正的 <exe>/clientExtensions
	m_root = QDir(QDir::tempPath()).filePath(
		QStringLiteral("dshhub-clientext-test-%1").arg(QCoreApplication::applicationPid()));
	QDir(m_root).removeRecursively();
	QVERIFY(QDir().mkpath(m_root));

	qputenv("DSHHUB_CLIENT_EXTENSION_DIR", m_root.toLocal8Bit());
	QCOMPARE(ClientExtension::extensionDirectory(), m_root);
}

void TestClientExtension::cleanupTestCase()
{
	unlockMainDll();
	QDir(m_root).removeRecursively();
	qunsetenv("DSHHUB_CLIENT_EXTENSION_DIR");
}

void TestClientExtension::installedNamesReflectsRegulation()
{
	makeExtensionDir(QStringLiteral("HasRegulation"), true);
	makeExtensionDir(QStringLiteral("NoRegulation"), false);

	QCOMPARE(ClientExtension::installedNames(), QStringList{ QStringLiteral("HasRegulation") });
}

void TestClientExtension::removeDeletesEverythingWhenUnlocked()
{
	const QString dir = makeExtensionDir(QStringLiteral("Unlocked"), true);

	QString error;
	QVERIFY2(ClientExtension::remove(QStringLiteral("Unlocked"), &error), qPrintable(error));
	QVERIFY(!QDir(dir).exists());
}

void TestClientExtension::removeMarksLeftoverWhenDllLocked()
{
	const QString dir = makeExtensionDir(QStringLiteral("Locked"), true);
	if (!lockMainDll(dir))
		QSKIP("本平台无法模拟被占用的 dll（只有 Windows 上才有这个文件锁语义）");

	QString error;
	const bool removed = ClientExtension::remove(QStringLiteral("Locked"), &error);

	// 返回值如实反映"目录没删干净"，并带回一句能给用户看的说明
	QVERIFY(!removed);
	QVERIFY(!error.isEmpty());

	// 判据已删 = 已卸载：不再算已安装，下次启动也不会被装载
	QVERIFY(!QFile::exists(QDir(dir).filePath(QStringLiteral("regulation.json5"))));
	QVERIFY(!ClientExtension::installedNames().contains(QStringLiteral("Locked")));

	// 残留目录 + 标记：留给下次启动的清扫
	QVERIFY(QDir(dir).exists());
	QVERIFY(QFile::exists(QDir(dir).filePath(QStringLiteral(".pending-removal"))));
}

void TestClientExtension::sweepOnNextStartClearsLeftover()
{
	// 接上一条用例：这里就是"宿主重启之后"的世界
	const QString dir = QDir(m_root).filePath(QStringLiteral("Locked"));
	QVERIFY(QDir(dir).exists());

	unlockMainDll();   // 等价于"上一个进程退出了，dll 不再被映射"

	// loadAll() 的第一件事就是清扫残留 —— 走的就是真实启动路径
	ClientExtension::loadAll();

	QVERIFY(!QDir(dir).exists());
}