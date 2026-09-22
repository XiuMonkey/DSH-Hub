#pragma once

// 按 highlight_rules.json 的规则给代码块做语法高亮；初始化时一次性加载规则并缓存结果。

#include <QHash>
#include <QMutex>
#include <QRegularExpression>
#include <QString>
#include <QVector>

class CodeHighlighter
{
public:
	static CodeHighlighter& instance();

	// 从 JSON 文件加载高亮规则；重复调用会先清空旧规则
	bool loadFromFile(const QString& filePath);

	// 对代码进行高亮，返回可直接放入 <pre> 的 HTML
	QString highlight(const QString& language, const QString& code) const;

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
	// ⚠️ 高亮会被渲染 worker 线程并发调用，规则/缓存读写统一加锁；用递归锁是因为 loadFromFile 持锁期间会调用同样加锁的 clearCache()。
	mutable QRecursiveMutex m_mutex;
};
