#pragma once

// ------------------------------------------------------------------
// CodeHighlighter.h
// ------------------------------------------------------------------
// 根据 highlight_rules.json 中的规则给代码块做语法高亮。
// 规则在初始化时一次性加载，高亮结果会缓存，避免重复处理。
// ------------------------------------------------------------------

#include <QHash>
#include <QRegularExpression>
#include <QString>
#include <QVector>

class CodeHighlighter
{
public:
    static CodeHighlighter &instance();

    // 从 JSON 文件加载高亮规则；重复调用会先清空旧规则
    bool loadFromFile(const QString &filePath);

    // 对代码进行高亮，返回可直接放入 <pre> 的 HTML
    QString highlight(const QString &language, const QString &code) const;

    // 清空高亮缓存（规则变化时调用）
    void clearCache();

private:
    CodeHighlighter() = default;

    struct Rule
    {
        QRegularExpression regex;
        QString color;
        bool bold = false;
        bool italic = false;
    };

    QHash<QString, QVector<Rule>> m_rules;
    mutable QHash<QString, QString> m_cache;
};
