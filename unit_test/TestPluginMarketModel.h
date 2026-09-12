#pragma once

// ------------------------------------------------------------------
// TestPluginMarketModel.h
// ------------------------------------------------------------------
// PluginMarketModel（从 PluginsManager 抽出的市场列表逻辑）单元测试：
// 解析、关键词过滤、分页切片，以及 AgentPresetService 的选中决策。
// ------------------------------------------------------------------

#include <QObject>

class TestPluginMarketModel : public QObject
{
	Q_OBJECT

private slots:
	// registry 解析
	void pluginsParsed();
	void pluginsWithoutNameSkipped();
	void displayDescriptionPrefersChinese();

	// 过滤
	void filterMatchesNameCategoryAndDescription();
	void filterIsCaseInsensitiveAndTrimmed();
	void emptyKeywordKeepsAll();

	// 分页
	void paginateClampsPageIndex();
	void paginateSlicesPage();
	void paginateAlwaysHasOnePage();

	// Agent 预设选中决策
	void presetParsedWithNameFallback();
	void presetSelectionPrefersSavedId();
	void presetSelectionFallsBackToServerDefault();
	void presetSelectionFallsBackToFirstWhenSavedMissing();
	void presetSelectionEmptyList();
};
