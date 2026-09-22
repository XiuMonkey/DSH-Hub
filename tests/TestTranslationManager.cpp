#include "TestTranslationManager.h"

#include "common/appearance/TranslationManager.h"

#include <QStringList>
#include <QTest>

void TestTranslationManager::initTestCase()
{
}

void TestTranslationManager::cleanupTestCase()
{
}

void TestTranslationManager::translationFileBase()
{
	QString base = Translation::translationFileBase(QStringLiteral("en"));
	QCOMPARE(base, QStringLiteral("dshhub_en"));
}

void TestTranslationManager::translationFileBaseEmpty()
{
	QString base = Translation::translationFileBase(QString());
	QVERIFY(base.isEmpty());
}

void TestTranslationManager::isDefaultLanguage()
{
	QVERIFY(Translation::isDefaultLanguage(QString()));
}

void TestTranslationManager::isDefaultLanguageZh()
{
	QVERIFY(Translation::isDefaultLanguage(QStringLiteral("zh")));
}

void TestTranslationManager::isDefaultLanguageZhCN()
{
	QVERIFY(Translation::isDefaultLanguage(QStringLiteral("zh_CN")));
}

void TestTranslationManager::isDefaultLanguageZhTW()
{
	QVERIFY(Translation::isDefaultLanguage(QStringLiteral("zh_TW")));
}

void TestTranslationManager::isDefaultLanguageEn()
{
	QVERIFY(!Translation::isDefaultLanguage(QStringLiteral("en")));
}

void TestTranslationManager::parseAvailableLanguagesEmpty()
{
	QStringList files;
	auto languages = Translation::parseAvailableLanguages(files);
	QVERIFY(languages.isEmpty());
}

void TestTranslationManager::parseAvailableLanguagesNoMatch()
{
	QStringList files;
	files << QStringLiteral("other.txt") << QStringLiteral("test.dat");
	auto languages = Translation::parseAvailableLanguages(files);
	QVERIFY(languages.isEmpty());
}

void TestTranslationManager::parseAvailableLanguagesSingle()
{
	QStringList files;
	files << QStringLiteral("dshhub_en.qm");
	auto languages = Translation::parseAvailableLanguages(files);
	QCOMPARE(languages.size(), 1);
	QCOMPARE(languages[0].code, QStringLiteral("en"));
}

void TestTranslationManager::parseAvailableLanguagesMultiple()
{
	QStringList files;
	files << QStringLiteral("dshhub_en.qm")
		<< QStringLiteral("dshhub_ja.qm")
		<< QStringLiteral("dshhub_fr.qm");
	auto languages = Translation::parseAvailableLanguages(files);
	QCOMPARE(languages.size(), 3);
	// 按名称排序，顺序取决于 QLocale 的本地化名称
	// 这里只检查数量和代码存在
	QStringList codes;
	for (const auto& lang : languages)
		codes << lang.code;
	QVERIFY(codes.contains(QStringLiteral("en")));
	QVERIFY(codes.contains(QStringLiteral("ja")));
	QVERIFY(codes.contains(QStringLiteral("fr")));
}

void TestTranslationManager::parseAvailableLanguagesDuplicate()
{
	QStringList files;
	files << QStringLiteral("dshhub_en.qm") << QStringLiteral("dshhub_en.qm");
	auto languages = Translation::parseAvailableLanguages(files);
	QCOMPARE(languages.size(), 1);
}

void TestTranslationManager::parseAvailableLanguagesInvalidExtension()
{
	QStringList files;
	files << QStringLiteral("dshhub_en.txt") << QStringLiteral("dshhub_en.json");
	auto languages = Translation::parseAvailableLanguages(files);
	QVERIFY(languages.isEmpty());
}