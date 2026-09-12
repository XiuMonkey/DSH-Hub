// ------------------------------------------------------------------
// TestPluginMarketModel.cpp
// ------------------------------------------------------------------
// 见 TestPluginMarketModel.h。
// ------------------------------------------------------------------

#include "TestPluginMarketModel.h"

#include "AgentPresetService.h"
#include "PluginMarketModel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

namespace
{
	QJsonArray parseArray(const char* json)
	{
		return QJsonDocument::fromJson(json).array();
	}

	QJsonObject parseObject(const char* json)
	{
		return QJsonDocument::fromJson(json).object();
	}
}

void TestPluginMarketModel::pluginsParsed()
{
	const QVector<MarketPlugin> plugins = PluginMarketModel::parsePlugins(parseArray(R"([
		{
			"name": "dsh-market-tools",
			"category": "工具",
			"url": "https://example.com/a.zip",
			"description": { "zh": "中文描述", "en": "English description" }
		}
	])"));

	QCOMPARE(plugins.size(), 1);
	QCOMPARE(plugins.at(0).name, QStringLiteral("dsh-market-tools"));
	QCOMPARE(plugins.at(0).category, QStringLiteral("工具"));
	QCOMPARE(plugins.at(0).url, QStringLiteral("https://example.com/a.zip"));
	QCOMPARE(plugins.at(0).descriptionZh, QStringLiteral("中文描述"));
	QCOMPARE(plugins.at(0).descriptionEn, QStringLiteral("English description"));
}

void TestPluginMarketModel::pluginsWithoutNameSkipped()
{
	const QVector<MarketPlugin> plugins = PluginMarketModel::parsePlugins(parseArray(R"([
		{ "category": "没有名字" },
		{ "name": "有名字" }
	])"));

	QCOMPARE(plugins.size(), 1);
	QCOMPARE(plugins.at(0).name, QStringLiteral("有名字"));
}

void TestPluginMarketModel::displayDescriptionPrefersChinese()
{
	MarketPlugin plugin;
	plugin.descriptionZh = QStringLiteral("中文");
	plugin.descriptionEn = QStringLiteral("English");
	QCOMPARE(plugin.displayDescription(), QStringLiteral("中文"));

	plugin.descriptionZh.clear();
	QCOMPARE(plugin.displayDescription(), QStringLiteral("English"));

	plugin.descriptionEn.clear();
	QVERIFY(plugin.displayDescription().isEmpty());
}

void TestPluginMarketModel::filterMatchesNameCategoryAndDescription()
{
	const QVector<MarketPlugin> plugins = PluginMarketModel::parsePlugins(parseArray(R"([
		{ "name": "alpha", "category": "工具", "description": { "zh": "文件处理" } },
		{ "name": "beta", "category": "主题", "description": { "en": "color themes" } }
	])"));

	QCOMPARE(PluginMarketModel::filter(plugins, QStringLiteral("alph")).size(), 1);
	QCOMPARE(PluginMarketModel::filter(plugins, QStringLiteral("工具")).size(), 1);
	QCOMPARE(PluginMarketModel::filter(plugins, QStringLiteral("文件")).size(), 1);
	QCOMPARE(PluginMarketModel::filter(plugins, QStringLiteral("color")).size(), 1);
	QCOMPARE(PluginMarketModel::filter(plugins, QStringLiteral("missing")).size(), 0);
}

void TestPluginMarketModel::filterIsCaseInsensitiveAndTrimmed()
{
	const QVector<MarketPlugin> plugins = PluginMarketModel::parsePlugins(parseArray(R"([
		{ "name": "AlphaBeta" }
	])"));

	QCOMPARE(PluginMarketModel::filter(plugins, QStringLiteral("ALPHABETA")).size(), 1);
	QCOMPARE(PluginMarketModel::filter(plugins, QStringLiteral("  alpha  ")).size(), 1);
}

void TestPluginMarketModel::emptyKeywordKeepsAll()
{
	const QVector<MarketPlugin> plugins = PluginMarketModel::parsePlugins(parseArray(R"([
		{ "name": "a" }, { "name": "b" }
	])"));

	QCOMPARE(PluginMarketModel::filter(plugins, QString()).size(), 2);
	QCOMPARE(PluginMarketModel::filter(plugins, QStringLiteral("   ")).size(), 2);
}

void TestPluginMarketModel::paginateClampsPageIndex()
{
	// 45 条、每页 20 → 3 页
	const PluginMarketModel::Page last = PluginMarketModel::paginate(45, 20, 99);
	QCOMPARE(last.count, 3);
	QCOMPARE(last.index, 2);

	const PluginMarketModel::Page first = PluginMarketModel::paginate(45, 20, -5);
	QCOMPARE(first.count, 3);
	QCOMPARE(first.index, 0);
}

void TestPluginMarketModel::paginateSlicesPage()
{
	const PluginMarketModel::Page page = PluginMarketModel::paginate(45, 20, 1);
	QCOMPARE(page.index, 1);
	QCOMPARE(page.begin, 20);
	QCOMPARE(page.end, 40);

	const PluginMarketModel::Page last = PluginMarketModel::paginate(45, 20, 2);
	QCOMPARE(last.begin, 40);
	QCOMPARE(last.end, 45);
}

void TestPluginMarketModel::paginateAlwaysHasOnePage()
{
	const PluginMarketModel::Page empty = PluginMarketModel::paginate(0, 20, 0);
	QCOMPARE(empty.count, 1);
	QCOMPARE(empty.begin, 0);
	QCOMPARE(empty.end, 0);
}

void TestPluginMarketModel::presetParsedWithNameFallback()
{
	const QVector<AgentPreset> presets = AgentPresetService::parsePresets(parseObject(R"({
		"presets": [
			{ "id": "p1", "name": "预设一", "isDefault": true },
			{ "id": "p2" },
			{ "name": "没有 id" }
		]
	})"));

	QCOMPARE(presets.size(), 2);
	QCOMPARE(presets.at(0).id, QStringLiteral("p1"));
	QCOMPARE(presets.at(0).name, QStringLiteral("预设一"));
	QVERIFY(presets.at(0).isDefault);
	QCOMPARE(presets.at(1).id, QStringLiteral("p2"));
	// 没有名字时回退为 id
	QCOMPARE(presets.at(1).name, QStringLiteral("p2"));
	QVERIFY(!presets.at(1).isDefault);
}

void TestPluginMarketModel::presetSelectionPrefersSavedId()
{
	const QVector<AgentPreset> presets = AgentPresetService::parsePresets(parseObject(R"({
		"presets": [
			{ "id": "p1", "isDefault": true },
			{ "id": "p2" }
		]
	})"));

	// 本地记住的 id 优先于服务端默认
	QCOMPARE(AgentPresetService::resolveSelectedId(presets, QStringLiteral("p2")), QStringLiteral("p2"));
}

void TestPluginMarketModel::presetSelectionFallsBackToServerDefault()
{
	const QVector<AgentPreset> presets = AgentPresetService::parsePresets(parseObject(R"({
		"presets": [
			{ "id": "p1" },
			{ "id": "p2", "isDefault": true }
		]
	})"));

	QCOMPARE(AgentPresetService::resolveSelectedId(presets, QString()), QStringLiteral("p2"));
}

void TestPluginMarketModel::presetSelectionFallsBackToFirstWhenSavedMissing()
{
	const QVector<AgentPreset> presets = AgentPresetService::parsePresets(parseObject(R"({
		"presets": [
			{ "id": "p1" },
			{ "id": "p2", "isDefault": true }
		]
	})"));

	// 记住的预设已不存在：回退到列表第一项（与原实现一致，而不是服务端默认）
	QCOMPARE(AgentPresetService::resolveSelectedId(presets, QStringLiteral("gone")), QStringLiteral("p1"));
}

void TestPluginMarketModel::presetSelectionEmptyList()
{
	QVERIFY(AgentPresetService::resolveSelectedId({}, QStringLiteral("p1")).isEmpty());
	QVERIFY(AgentPresetService::parsePresets(parseObject(R"({ "presets": [] })")).isEmpty());
}