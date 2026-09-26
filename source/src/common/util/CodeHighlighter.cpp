#include "common/util/CodeHighlighter.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpressionMatch>

CodeHighlighter& CodeHighlighter::instance()
{
	static CodeHighlighter highlighter;
	return highlighter;
}

bool CodeHighlighter::loadFromFile(const QString& filePath)
{
	QFile file(filePath);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return false;

	const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
	file.close();
	if (!doc.isObject())
		return false;

	QMutexLocker locker(&m_mutex);
	m_rules.clear();
	clearCache();

	const QJsonObject root = doc.object();
	const QJsonObject languages = root.value(QStringLiteral("languages")).toObject();
	for (auto it = languages.begin(); it != languages.end(); ++it) {
		const QString language = it.key();
		const QJsonArray rulesArray = it.value().toArray();

		QVector<Rule> rules;
		for (const auto& value : rulesArray) {
			const QJsonObject ruleObj = value.toObject();
			const QString pattern = ruleObj.value(QStringLiteral("pattern")).toString();
			if (pattern.isEmpty())
				continue;

			Rule rule;
			rule.regex = QRegularExpression(pattern);
			rule.color = ruleObj.value(QStringLiteral("color")).toString(QStringLiteral("#000000"));
			rule.bold = ruleObj.value(QStringLiteral("bold")).toBool(false);
			rule.italic = ruleObj.value(QStringLiteral("italic")).toBool(false);
			rules.append(rule);
		}

		m_rules.insert(language, rules);
	}

	return true;
}

void CodeHighlighter::clearCache()
{
	QMutexLocker locker(&m_mutex);
	m_cache.clear();
}

QString CodeHighlighter::highlight(const QString& language, const QString& code) const
{
	QMutexLocker locker(&m_mutex);

	const QString key = language + QLatin1Char('\n') + code;
	const auto cached = m_cache.constFind(key);
	if (cached != m_cache.constEnd())
		return cached.value();

	const QString lang = language.isEmpty() ? QStringLiteral("default") : language;
	const QVector<Rule> rules = m_rules.value(lang, m_rules.value(QStringLiteral("default")));

	QString html;
	int pos = 0;
	const int codeLength = code.length();

	// 逐位置贪心：每轮在剩余文本里找最早、同位置最长的那条规则；前段原样转义，命中段套 span
	while (pos < codeLength) {
		int bestPos = -1;
		int bestLen = 0;
		const Rule* bestRule = nullptr;

		for (const Rule& rule : rules) {
			const QRegularExpressionMatch match = rule.regex.match(code, pos);
			if (!match.hasMatch())
				continue;

			const int matchPos = match.capturedStart();
			const int matchLen = match.capturedLength();
			if (matchLen <= 0)
				continue;

			if (bestPos < 0 || matchPos < bestPos
				|| (matchPos == bestPos && matchLen > bestLen)) {
				bestPos = matchPos;
				bestLen = matchLen;
				bestRule = &rule;
			}
		}

		if (!bestRule) {
			html += code.mid(pos).toHtmlEscaped();
			break;
		}

		html += code.mid(pos, bestPos - pos).toHtmlEscaped();

		const QString matched = code.mid(bestPos, bestLen).toHtmlEscaped();
		QString style = QStringLiteral("color:%1;").arg(bestRule->color);
		if (bestRule->bold)
			style += QStringLiteral("font-weight:bold;");
		if (bestRule->italic)
			style += QStringLiteral("font-style:italic;");
		html += QStringLiteral("<span style=\"") + style + QStringLiteral("\">") + matched + QStringLiteral("</span>");

		pos = bestPos + bestLen;
	}

	m_cache.insert(key, html);
	return html;
}
