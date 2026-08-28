#pragma once

// ------------------------------------------------------------------
// TestCodeHighlighter.h
// ------------------------------------------------------------------
// CodeHighlighter 语法高亮功能的单元测试。
// ------------------------------------------------------------------

#include <QObject>
#include <QTemporaryDir>
#include <QString>

class TestCodeHighlighter : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tempDir;
    QString m_rulesPath;

private slots:
    void initTestCase();
    void cleanupTestCase();

    void loadFromFile_nonExistentReturnsFalse();
    void loadFromFile_validJson();
    void loadFromFile_invalidJson();

    void highlight_unknownLanguageUsesDefault();
    void highlight_htmlEscapesPlainText();
    void highlight_keywordSpan();
    void highlight_sameInputReturnsConsistentResult();
};