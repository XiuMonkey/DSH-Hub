#include "TestCodeHighlighter.h"

#include "CodeHighlighter.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

void TestCodeHighlighter::initTestCase()
{
    QVERIFY(m_tempDir.isValid());

    m_rulesPath = m_tempDir.filePath(QStringLiteral("highlight_rules.json"));
    QFile file(m_rulesPath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(R"json({
        "languages": {
            "default": [
                { "pattern": "\\b(hello|world)\\b", "color": "#ff0000", "bold": true },
                { "pattern": "\\d+", "color": "#00ff00" }
            ],
            "cpp": [
                { "pattern": "\\b(int|return)\\b", "color": "#0000ff" }
            ]
        }
    })json");
    file.close();
}

void TestCodeHighlighter::cleanupTestCase()
{
    m_tempDir.remove();
}

void TestCodeHighlighter::loadFromFile_nonExistentReturnsFalse()
{
    CodeHighlighter &highlighter = CodeHighlighter::instance();
    QVERIFY(!highlighter.loadFromFile(m_tempDir.filePath(QStringLiteral("not_exist.json"))));
}

void TestCodeHighlighter::loadFromFile_validJson()
{
    CodeHighlighter &highlighter = CodeHighlighter::instance();
    QVERIFY(highlighter.loadFromFile(m_rulesPath));
}

void TestCodeHighlighter::loadFromFile_invalidJson()
{
    const QString invalidPath = m_tempDir.filePath(QStringLiteral("invalid.json"));
    QFile file(invalidPath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("this is not json");
    file.close();

    CodeHighlighter &highlighter = CodeHighlighter::instance();
    QVERIFY(!highlighter.loadFromFile(invalidPath));
}

void TestCodeHighlighter::highlight_unknownLanguageUsesDefault()
{
    CodeHighlighter &highlighter = CodeHighlighter::instance();
    QVERIFY(highlighter.loadFromFile(m_rulesPath));

    const QString html = highlighter.highlight(QStringLiteral("unknown"), QStringLiteral("hello"));
    QCOMPARE(html, QStringLiteral("<span style=\"color:#ff0000;font-weight:bold;\">hello</span>"));
}

void TestCodeHighlighter::highlight_htmlEscapesPlainText()
{
    CodeHighlighter &highlighter = CodeHighlighter::instance();
    QVERIFY(highlighter.loadFromFile(m_rulesPath));

    const QString html = highlighter.highlight(QStringLiteral("plain"), QStringLiteral("<tag> & \"quotes\""));
    QVERIFY(html.contains(QStringLiteral("&lt;tag&gt; &amp; &quot;quotes&quot;")));
}

void TestCodeHighlighter::highlight_keywordSpan()
{
    CodeHighlighter &highlighter = CodeHighlighter::instance();
    QVERIFY(highlighter.loadFromFile(m_rulesPath));

    const QString html = highlighter.highlight(QStringLiteral("cpp"), QStringLiteral("int main() { return 0; }"));
    QVERIFY(html.contains(QStringLiteral("<span style=\"color:#0000ff;\">int</span>")));
    QVERIFY(html.contains(QStringLiteral("<span style=\"color:#0000ff;\">return</span>")));
}

void TestCodeHighlighter::highlight_sameInputReturnsConsistentResult()
{
    CodeHighlighter &highlighter = CodeHighlighter::instance();
    QVERIFY(highlighter.loadFromFile(m_rulesPath));

    const QString first = highlighter.highlight(QStringLiteral("default"), QStringLiteral("hello 42"));
    const QString second = highlighter.highlight(QStringLiteral("default"), QStringLiteral("hello 42"));
    QCOMPARE(first, second);
    QVERIFY(first.contains(QStringLiteral("<span style=\"color:#ff0000;font-weight:bold;\">hello</span>")));
    QVERIFY(first.contains(QStringLiteral("<span style=\"color:#00ff00;\">42</span>")));
}