#pragma once

// ------------------------------------------------------------------
// TestModelSelection.h
// ------------------------------------------------------------------
// ModelSelectionService（模型目录 / 思考档位 / 服务端模型视图）单元测试：
// 覆盖 session.models 与 llm.models 回包的解析、名称回退、
// “当前档位 / 可用档位 / 档位名”这些派生查询，以及
// llm.providers + settings.describe 的解析、模型行 join、
// 新增模型时的条目拼装与整份数组 upsert，以及获取模型
//（llm/discoverModels）的回包解析与请求拼装。
// ------------------------------------------------------------------

#include <QObject>

class TestModelSelection : public QObject
{
	Q_OBJECT

private slots:
	// 解析
	void directoryParsed();
	void idsRequiredAndNamesFallBack();
	void modelWithoutReasoningHasNoLevels();
	void minimalPayloadParsed();

	// 派生查询
	void currentLevelsMatchSelectedModel();
	void currentLevelPrefersExplicitEffort();
	void currentLevelFallsBackToAdapterDefault();
	void currentLevelNullForUnknownModel();
	void currentLevelNullForUnknownEffort();
	void currentLevelNameIsEmptyWithoutMetadata();

	// 通用兜底档位（适配器未公布时仍可调档）
	void selectableLevelsFallBackToGenericFour();
	void fallbackLevelResolvesInCurrentLevel();
	void fallbackIsOffWhenAdapterPublishesLevels();

	// 服务端视图：llm.providers / settings.describe
	void providersParsed();
	void namespacesParsed();
	void catalogGroupsAndFailuresParsed();
	void configuredModelsUseProfilePath();
	void userDeclaresModelsDetectsUserLayerOnly();
	void modelsForWritePrefersUserLayer();
	void configuredModelFieldsParsed();

	// join
	void modelInfosJoinCatalogWithSettings();
	void modelInfosAppendDeclaredOnlyModels();
	void modelInfosWithoutSettingsStillListCatalog();

	// 新增模型
	void modelEntryForPiAiCarriesReasoningEfforts();
	void modelEntryForDeepSeekCarriesInputModalities();
	void modelEntryOmitsEmptyOptionalFields();
	void upsertAppendsNewModel();
	void upsertReplacesSameIdInPlace();

	// 删除模型（右键菜单用）
	void removeDropsMatchingIdOnly();
	void removeIsNoOpForUnknownId();

	// 凭据（API Key）
	void keyRefDerivedFromRoute();
	void keyRefPrefersProfileApiKeyEnv();
	void credentialStatusParsed();

	// 获取模型（llm/discoverModels）
	void discoveredModelsParsed();
	void discoveredModelsSkipsRowsWithoutId();
	void discoveryRequestCarriesRouteAndProfile();
};
