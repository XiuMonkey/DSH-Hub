#pragma once

// 插件市场列表的纯数据模型：把 registry 的 JSON 解析成结构化条目，并提供关键词过滤与分页切片；UI 只负责画卡片。

#include <QJsonArray>
#include <QString>
#include <QVector>

struct MarketPlugin
{
	QString name;
	QString category;
	QString url;
	QString descriptionZh;
	QString descriptionEn;

	// 展示用描述：中文优先，缺省回落英文
	QString displayDescription() const;

	// 名称/分类/中英文描述任一命中关键词即算匹配（空关键词视为全部匹配）
	bool matches(const QString& keyword) const;
};

namespace PluginMarketModel
{
	// 解析 registry.plugins 数组；缺少 name 的条目会被丢弃
	QVector<MarketPlugin> parsePlugins(const QJsonArray& registryPlugins);

	QVector<MarketPlugin> filter(const QVector<MarketPlugin>& plugins, const QString& keyword);

	// 一页的切片结果（index 从 0 开始，已按 total/pageSize 收敛到合法范围）
	struct Page
	{
		int index = 0;
		int count = 1;
		int begin = 0;
		int end = 0;
	};

	Page paginate(int total, int pageSize, int requestedPage);
}
