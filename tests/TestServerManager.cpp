#include "TestServerManager.h"

#include "core/ServerManager.h"

#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QTest>

void TestServerManager::initTestCase()
{
}

void TestServerManager::cleanupTestCase()
{
}

void TestServerManager::defaultState()
{
	ServerManager manager;
	// 默认状态应该不是重启中
	QVERIFY(!manager.isRestarting());
	// dshHome 应该为空或有效路径
	QString home = manager.dshHome();
	// 不检查具体值，只确保不会崩溃
	QVERIFY(true);
}

void TestServerManager::isRestartingDefault()
{
	ServerManager manager;
	QVERIFY(!manager.isRestarting());
}

void TestServerManager::dshHomeDefault()
{
	ServerManager manager;
	QString home = manager.dshHome();
	// 不检查具体值，只确保不会崩溃
	QVERIFY(true);
}

// 出厂态必须是"不随附任何模型"：服务端的适配器在 models 缺席时会公布自带的
// 默认目录（deepseek-flash 等 4 条），只有显式写空数组才压得住。
// 这里锁三件事：写出空数组、重复调用幂等、已存在的文件（用户配置）绝不被动。
void TestServerManager::factorySettingsSuppressBundledModels()
{
	// 空路径不该去拼 "/settings.yaml"
	QVERIFY(!ServerManager::ensureFactorySettings(QString()));

	QTemporaryDir dir;
	QVERIFY(dir.isValid());

	const QString path = dir.path() + QStringLiteral("/settings.yaml");
	QVERIFY(!QFile::exists(path));

	QVERIFY(ServerManager::ensureFactorySettings(dir.path()));
	QVERIFY(QFile::exists(path));

	QFile first(path);
	QVERIFY(first.open(QIODevice::ReadOnly | QIODevice::Text));
	const QString written = QString::fromUtf8(first.readAll());
	first.close();

	QVERIFY(written.contains(QStringLiteral("llm-deepseek:")));
	QVERIFY(written.contains(QStringLiteral("models: []")));

	// 幂等：再叫一次不该改动内容
	QVERIFY(ServerManager::ensureFactorySettings(dir.path()));
	QFile second(path);
	QVERIFY(second.open(QIODevice::ReadOnly | QIODevice::Text));
	QCOMPARE(QString::fromUtf8(second.readAll()), written);
	second.close();

	// 用户改过（把空数组删掉、加了自己的模型）：一个字都不能被覆盖
	const QString userText = QStringLiteral("llm-deepseek:\n  models:\n    - id: mine\n");
	QFile user(path);
	QVERIFY(user.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate));
	user.write(userText.toUtf8());
	user.close();

	QVERIFY(ServerManager::ensureFactorySettings(dir.path()));
	QFile kept(path);
	QVERIFY(kept.open(QIODevice::ReadOnly | QIODevice::Text));
	const QString after = QString::fromUtf8(kept.readAll());
	kept.close();

	QCOMPARE(after, userText);
	QVERIFY(!after.contains(QStringLiteral("models: []")));
}