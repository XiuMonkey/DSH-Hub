#pragma once

// ------------------------------------------------------------------
// TestTranslationManager.h
// ------------------------------------------------------------------
// TranslationManager（多语言管理）的单元测试。
// ------------------------------------------------------------------

#include <QObject>

class TestTranslationManager : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase();
	void cleanupTestCase();

	void translationFileBase();
	void translationFileBaseEmpty();
	void isDefaultLanguage();
	void isDefaultLanguageZh();
	void isDefaultLanguageZhCN();
	void isDefaultLanguageZhTW();
	void isDefaultLanguageEn();
	void parseAvailableLanguagesEmpty();
	void parseAvailableLanguagesNoMatch();
	void parseAvailableLanguagesSingle();
	void parseAvailableLanguagesMultiple();
	void parseAvailableLanguagesDuplicate();
	void parseAvailableLanguagesInvalidExtension();
};
