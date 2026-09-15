// ------------------------------------------------------------------
// TestModelSelection.cpp
// ------------------------------------------------------------------
// 见 TestModelSelection.h。
// ------------------------------------------------------------------

#include "TestModelSelection.h"

#include "ModelSelectionService.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

namespace
{
	QJsonObject parseObject(const char* json)
	{
		return QJsonDocument::fromJson(json).object();
	}

	// dsh 0.1.5：llm/listConfigurableProviders 返回的是**裸数组**
	// （[{provider, displayName, settingsNs, settingsPath, …}]），
	// 而这里的用例数据仍是旧形状 { "providers": [ … ] }，用这个适配函数取出来
	// 再喂给生产解析函数，省得把每个用例的 JSON 都改一遍。
	QVector<ConfigurableProvider> parseProvidersJson(const char* json)
	{
		return ModelSelectionService::parseProviders(
			parseObject(json).value(QStringLiteral("providers")).toArray());
	}

	// 一份贴近 harness 实际返回的 session/modelCatalog 回包（0.1.5 形状）：
	// default 是部署默认选择、routableProviders 是当前可路由的提供方；
	// deepseek 提供方公布 off/low/high/max，默认 high；另一个模型不公布推理元数据
	const char* const kDirectoryJson = R"({
		"default": { "provider": "deepseek", "model": "deepseek-v4-flash", "reasoningEffort": "high" },
		"routableProviders": [ "deepseek", "gateway" ],
		"groups": [
			{
				"id": "deepseek",
				"name": "DeepSeek",
				"models": [
					{
						"id": "deepseek-v4-flash",
						"name": "DeepSeek V4 Flash",
						"description": "快速模型",
						"reasoning": {
							"efforts": [
								{ "id": "off", "name": "Off" },
								{ "id": "low", "name": "Low", "description": "轻量思考" },
								{ "id": "high", "name": "High", "description": "深度思考" },
								{ "id": "max", "name": "Max" }
							],
							"defaultEffort": "high"
						}
					},
					{ "id": "no-reasoning", "name": "No Reasoning" }
				]
			},
			{
				"id": "gateway",
				"models": [ { "id": "gpt-x" } ]
			}
		],
		"failures": [ { "id": "broken", "message": "boom" } ]
	})";
}

void TestModelSelection::directoryParsed()
{
	const SessionModelDirectory directory = ModelSelectionService::parseDirectory(parseObject(kDirectoryJson));

	QCOMPARE(directory.current.provider, QStringLiteral("deepseek"));
	QCOMPARE(directory.current.model, QStringLiteral("deepseek-v4-flash"));
	QCOMPARE(directory.current.reasoningEffort, QStringLiteral("high"));
	QVERIFY(directory.routable);
	QCOMPARE(directory.groups.size(), 2);

	const ModelProviderGroup& deepseek = directory.groups.at(0);
	QCOMPARE(deepseek.id, QStringLiteral("deepseek"));
	QCOMPARE(deepseek.name, QStringLiteral("DeepSeek"));
	QCOMPARE(deepseek.models.size(), 2);

	const ModelOption& flash = deepseek.models.at(0);
	QCOMPARE(flash.id, QStringLiteral("deepseek-v4-flash"));
	QCOMPARE(flash.name, QStringLiteral("DeepSeek V4 Flash"));
	QVERIFY(flash.hasReasoning);
	QCOMPARE(flash.reasoning.levels.size(), 4);
	QCOMPARE(flash.reasoning.levels.at(1).id, QStringLiteral("low"));
	QCOMPARE(flash.reasoning.levels.at(1).description, QStringLiteral("轻量思考"));
	QCOMPARE(flash.reasoning.levels.at(2).description, QStringLiteral("深度思考"));
	// 没有 description 的档位保持为空（界面只显示档位名）
	QVERIFY(flash.reasoning.levels.at(0).description.isEmpty());
	QVERIFY(flash.reasoning.levels.at(3).description.isEmpty());
	QCOMPARE(flash.reasoning.defaultLevelId, QStringLiteral("high"));
}

void TestModelSelection::idsRequiredAndNamesFallBack()
{
	const SessionModelDirectory directory = ModelSelectionService::parseDirectory(parseObject(kDirectoryJson));

	// 提供方没给 name -> 回退为 id
	QCOMPARE(directory.groups.at(1).name, QStringLiteral("gateway"));
	// 模型没给 name -> 回退为 id
	QCOMPARE(directory.groups.at(1).models.at(0).name, QStringLiteral("gpt-x"));

	// 缺 id 的分组/模型/档位被丢弃
	const SessionModelDirectory sparse = ModelSelectionService::parseDirectory(parseObject(R"({
		"groups": [
			{ "name": "no id" },
			{ "id": "g", "models": [ { "name": "no id" }, { "id": "m", "reasoning": { "efforts": [ { "name": "no id" } ] } } ] }
		]
	})"));

	QCOMPARE(sparse.groups.size(), 1);
	QCOMPARE(sparse.groups.at(0).models.size(), 1);
	QVERIFY(sparse.groups.at(0).models.at(0).reasoning.levels.isEmpty());
	// 公布过 reasoning 但档位全被丢弃 -> 视为没有元数据，界面应隐藏控件
	QVERIFY(!sparse.groups.at(0).models.at(0).hasReasoning);
}

void TestModelSelection::modelWithoutReasoningHasNoLevels()
{
	const SessionModelDirectory directory = ModelSelectionService::parseDirectory(parseObject(kDirectoryJson));

	const ModelOption* option = directory.findModel(QStringLiteral("deepseek"), QStringLiteral("no-reasoning"));
	QVERIFY(option != nullptr);
	QVERIFY(!option->hasReasoning);
	QVERIFY(option->reasoning.levels.isEmpty());

	// 目录里没有的模型（适配器仍可服务未公布的模型）
	QVERIFY(directory.findModel(QStringLiteral("deepseek"), QStringLiteral("unknown")) == nullptr);
	QVERIFY(directory.findModel(QStringLiteral("unknown"), QStringLiteral("deepseek-v4-flash")) == nullptr);
}

void TestModelSelection::minimalPayloadParsed()
{
	// 空回包（没有可路由提供方）不应崩，也不该造出任何分组
	const SessionModelDirectory directory = ModelSelectionService::parseDirectory(parseObject(R"({ "routableProviders": [] })"));
	QVERIFY(directory.groups.isEmpty());
	QVERIFY(!directory.routable);
	QVERIFY(!directory.current.isValid());
}

void TestModelSelection::currentLevelsMatchSelectedModel()
{
	SessionModelDirectory directory = ModelSelectionService::parseDirectory(parseObject(kDirectoryJson));
	QCOMPARE(directory.currentLevels().size(), 4);
	QCOMPARE(directory.currentLevels().at(3).id, QStringLiteral("max"));

	// 切到不公布推理元数据的模型：适配器一个档位都没公布（事实），
	// 但界面仍按通用四档提供，所以显式选中过的档位依然能显示出来。
	directory.current.model = QStringLiteral("no-reasoning");
	QVERIFY(directory.currentLevels().isEmpty());
	QVERIFY(directory.usesFallbackLevels());
	QCOMPARE(directory.selectableLevels().size(), 4);

	// kDirectoryJson 里 current 是显式的 high -> 落在通用四档里，显示为 High
	QVERIFY(directory.currentLevel() != nullptr);
	QCOMPARE(directory.currentLevelName(), QStringLiteral("High"));

	// 档位清空后：适配器没给默认值，就不替用户瞎选，chip 只显示模型名
	directory.current.reasoningEffort.clear();
	QVERIFY(directory.currentLevel() == nullptr);
	QVERIFY(directory.currentLevelName().isEmpty());
}

void TestModelSelection::currentLevelPrefersExplicitEffort()
{
	SessionModelDirectory directory = ModelSelectionService::parseDirectory(parseObject(kDirectoryJson));
	directory.current.reasoningEffort = QStringLiteral("max");

	const ReasoningLevel* level = directory.currentLevel();
	QVERIFY(level != nullptr);
	QCOMPARE(level->id, QStringLiteral("max"));
	QCOMPARE(directory.currentLevelName(), QStringLiteral("Max"));
}

void TestModelSelection::currentLevelFallsBackToAdapterDefault()
{
	SessionModelDirectory directory = ModelSelectionService::parseDirectory(parseObject(kDirectoryJson));
	// 省略档位 = 提供方默认；展示层显示适配器公布的默认档位
	directory.current.reasoningEffort.clear();

	const ReasoningLevel* level = directory.currentLevel();
	QVERIFY(level != nullptr);
	QCOMPARE(level->id, QStringLiteral("high"));
	QCOMPARE(directory.currentLevelName(), QStringLiteral("High"));
}

void TestModelSelection::currentLevelNullForUnknownModel()
{
	SessionModelDirectory directory = ModelSelectionService::parseDirectory(parseObject(kDirectoryJson));
	directory.current.model = QStringLiteral("not-in-catalog");

	QVERIFY(directory.currentLevels().isEmpty());
	QVERIFY(directory.currentLevel() == nullptr);
}

void TestModelSelection::currentLevelNullForUnknownEffort()
{
	SessionModelDirectory directory = ModelSelectionService::parseDirectory(parseObject(kDirectoryJson));
	// 档位不在该模型公布的能力里：不应硬套默认值，控件会退回“默认”显示
	directory.current.reasoningEffort = QStringLiteral("xhigh");

	QVERIFY(directory.currentLevel() == nullptr);
	QVERIFY(directory.currentLevelName().isEmpty());
	QCOMPARE(directory.currentLevels().size(), 4);
}

void TestModelSelection::currentLevelNameIsEmptyWithoutMetadata()
{
	const SessionModelDirectory directory = ModelSelectionService::parseDirectory(parseObject(R"({
		"default": { "provider": "p", "model": "m" },
		"groups": [ { "id": "p", "models": [ { "id": "m" } ] } ]
	})"));

	QVERIFY(directory.current.isValid());
	QVERIFY(directory.currentLevels().isEmpty());
	QVERIFY(directory.currentLevelName().isEmpty());
}

// ------------------------------------------------------------------
// 通用兜底档位：适配器没公布时，界面仍提供 off/low/high/max 四档
// ------------------------------------------------------------------

namespace
{
	// 一个不公布任何推理元数据的模型的会话目录（配一个公布档位的模型作对照）
	const char* const kSparseLevelsJson = R"({
		"default": { "provider": "p", "model": "m" },
		"routableProviders": [ "p" ],
		"groups": [
			{
				"id": "p",
				"name": "P",
				"models": [
					{ "id": "m" },
					{ "id": "with-levels", "reasoning": { "efforts": [ { "id": "off", "name": "Off" }, { "id": "high", "name": "High" } ], "defaultEffort": "high" } }
				]
			}
		]
	})";
}

void TestModelSelection::selectableLevelsFallBackToGenericFour()
{
	const SessionModelDirectory directory =
		ModelSelectionService::parseDirectory(parseObject(kSparseLevelsJson));

	// 原始事实：适配器确实什么都没公布
	QVERIFY(directory.currentLevels().isEmpty());
	QVERIFY(directory.usesFallbackLevels());

	// 界面可选的档位 = 通用四档
	const QVector<ReasoningLevel> levels = directory.selectableLevels();
	QCOMPARE(levels.size(), 4);
	QCOMPARE(levels.at(0).id, QStringLiteral("off"));
	QCOMPARE(levels.at(1).id, QStringLiteral("low"));
	QCOMPARE(levels.at(2).id, QStringLiteral("high"));
	QCOMPARE(levels.at(3).id, QStringLiteral("max"));
	// 展示名不能为空（否则菜单里是空行）
	for (const ReasoningLevel& level : levels)
		QVERIFY(!level.name.isEmpty());
}

void TestModelSelection::fallbackLevelResolvesInCurrentLevel()
{
	SessionModelDirectory directory =
		ModelSelectionService::parseDirectory(parseObject(kSparseLevelsJson));

	// 未指定档位：适配器没给默认值，所以拿不到当前档位（不替用户瞎选一个）
	QVERIFY(directory.currentLevel() == nullptr);
	QVERIFY(directory.currentLevelName().isEmpty());

	// 用户从通用档位里挑了 High：chip 必须能显示出来
	directory.current.reasoningEffort = QStringLiteral("high");
	const ReasoningLevel* level = directory.currentLevel();
	QVERIFY(level != nullptr);
	QCOMPARE(level->id, QStringLiteral("high"));
	QCOMPARE(level->name, QStringLiteral("High"));
	QCOMPARE(directory.currentLevelName(), QStringLiteral("High"));

	// 兜底四档之外的档位仍然解析不到
	directory.current.reasoningEffort = QStringLiteral("xhigh");
	QVERIFY(directory.currentLevel() == nullptr);
}

void TestModelSelection::fallbackIsOffWhenAdapterPublishesLevels()
{
	SessionModelDirectory directory =
		ModelSelectionService::parseDirectory(parseObject(kSparseLevelsJson));

	// 公布了档位的模型：用公布的那份，不掺通用四档
	directory.current.model = QStringLiteral("with-levels");
	QVERIFY(!directory.usesFallbackLevels());
	QCOMPARE(directory.selectableLevels().size(), 2);
	QCOMPARE(directory.selectableLevels().at(1).id, QStringLiteral("high"));

	// 目录里根本没有该模型时，既没有公布档位也没有当前档位
	directory.current.model = QStringLiteral("not-in-catalog");
	QVERIFY(directory.currentLevels().isEmpty());
	QVERIFY(directory.currentLevel() == nullptr);
	QVERIFY(directory.usesFallbackLevels());
	// 但兜底档位仍可列出来（界面靠它才不至于无从下手）
	QCOMPARE(directory.selectableLevels().size(), 4);
}

// ------------------------------------------------------------------
// 服务端视图：llm.providers / settings.describe / llm.models
// ------------------------------------------------------------------
// 下面两份回包取自真实 harness 的裁剪版：
//   deepseek-official 整节即 profile（settingsPath 为空），
//   siliconflow-cn 挂在 pi-ai 的 providers.<路由> 下。

namespace
{
	const char* const kProvidersJson = R"({
		"providers": [
			{ "provider": "deepseek-official", "displayName": "DeepSeek",
			  "settingsNs": "llm-deepseek", "settingsPath": [], "active": true },
			{ "provider": "siliconflow-cn", "displayName": "siliconflow-cn",
			  "settingsNs": "llm-pi-ai", "settingsPath": ["providers", "siliconflow-cn"],
			  "active": true, "declared": true },
			{ "provider": "agnes", "displayName": "agnes",
			  "settingsNs": "llm-pi-ai", "settingsPath": ["providers", "agnes"],
			  "active": false }
		]
	})";

	// 只有 siliconflow-cn 在用户层自己写了 models 列表；
	// deepseek-official 的 models 来自随附配置（user 为空）。
	const char* const kNamespacesJson = R"({
		"writable": true,
		"hasDocument": true,
		"namespaces": [
			{
				"ns": "llm-deepseek",
				"value": {
					"apiKeyEnv": "DEEPSEEK_API_KEY",
					"maxTokens": 256000,
					"models": [
						{ "id": "deepseek-v4-flash", "name": "DeepSeek-V4-Flash",
						  "contextWindow": 1000000, "inputModalities": ["text"] }
					]
				},
				"user": {},
				"applies": "live",
				"secrets": [],
				"revision": 3
			},
			{
				"ns": "llm-pi-ai",
				"value": {
					"providers": {
						"siliconflow-cn": {
							"baseURL": "https://api.siliconflow.cn/v1",
							"models": [
								{ "id": "deepseek-ai/DeepSeek-V4-Flash", "reasoningEfforts": { "off": null, "high": "high" } },
								{ "id": "Qwen/Qwen-Image", "maxTokens": 8192, "input": ["text", "image"] }
							]
						},
						"agnes": {
							"baseURL": "https://apihub.agnes-ai.com/v1",
							"models": [ { "id": "agnes-image-2.5-flash", "reasoningEfforts": false } ]
						}
					}
				},
				"user": {
					"providers": {
						"siliconflow-cn": {
							"models": [ { "id": "deepseek-ai/DeepSeek-V4-Flash" }, { "id": "Qwen/Qwen-Image" } ]
						}
					}
				},
				"applies": "live",
				"secrets": [],
				"revision": 7
			}
		]
	})";

	const char* const kCatalogJson = R"({
		"groups": [
			{
				"id": "deepseek-official",
				"name": "DeepSeek",
				"models": [
					{ "id": "deepseek-v4-flash", "name": "DeepSeek-V4-Flash",
					  "reasoning": { "efforts": [ { "id": "off", "name": "Off" }, { "id": "high", "name": "High" } ], "defaultEffort": "high" } }
				]
			}
		],
		"failures": [ { "id": "broken", "name": "Broken", "message": "boom" } ]
	})";
}

void TestModelSelection::providersParsed()
{
	const QVector<ConfigurableProvider> providers =
		parseProvidersJson(kProvidersJson);

	QCOMPARE(providers.size(), 3);

	// deepseek 系：整节即 profile，settingsPath 为空
	QCOMPARE(providers.at(0).provider, QStringLiteral("deepseek-official"));
	QCOMPARE(providers.at(0).displayName, QStringLiteral("DeepSeek"));
	QCOMPARE(providers.at(0).settingsNs, QStringLiteral("llm-deepseek"));
	QVERIFY(providers.at(0).settingsPath.isEmpty());
	QVERIFY(providers.at(0).active);
	QVERIFY(!providers.at(0).declared);

	// pi-ai 系：profile 挂在 providers.<路由> 下
	QCOMPARE(providers.at(1).settingsPath,
		QStringList({ QStringLiteral("providers"), QStringLiteral("siliconflow-cn") }));
	QVERIFY(providers.at(1).declared);

	// 没给 displayName 时回退为 provider；active 缺席视为 false
	QCOMPARE(providers.at(2).displayName, QStringLiteral("agnes"));
	QVERIFY(!providers.at(2).active);

	// 缺 provider 的条目被丢弃
	const QVector<ConfigurableProvider> sparse = parseProvidersJson(R"({
		"providers": [ { "displayName": "no id" }, { "provider": "ok" } ]
	})");
	QCOMPARE(sparse.size(), 1);
	QCOMPARE(sparse.at(0).provider, QStringLiteral("ok"));
}

void TestModelSelection::namespacesParsed()
{
	bool writable = false;
	const QVector<SettingsNamespace> namespaces =
		ModelSelectionService::parseNamespaces(parseObject(kNamespacesJson), &writable);

	QVERIFY(writable);
	QCOMPARE(namespaces.size(), 2);

	const SettingsNamespace& deepseek = namespaces.at(0);
	QCOMPARE(deepseek.ns, QStringLiteral("llm-deepseek"));
	QCOMPARE(deepseek.revision, 3);
	QVERIFY(deepseek.user.isEmpty());
	QCOMPARE(deepseek.objectAt({}).value(QStringLiteral("models")).toArray().size(), 1);

	const SettingsNamespace& piAi = namespaces.at(1);
	QCOMPARE(piAi.revision, 7);
	// value 是分层合并后的生效值：baseURL 只在 value 里
	QCOMPARE(piAi.objectAt({ QStringLiteral("providers"), QStringLiteral("siliconflow-cn") })
		.value(QStringLiteral("baseURL")).toString(),
		QStringLiteral("https://api.siliconflow.cn/v1"));

	// 路径走空时返回空对象，而不是崩溃或返回根
	QVERIFY(piAi.objectAt({ QStringLiteral("providers"), QStringLiteral("nope") }).isEmpty());

	// writable 缺席时视为不可写
	bool absent = true;
	ModelSelectionService::parseNamespaces(parseObject(R"({ "namespaces": [] })"), &absent);
	QVERIFY(!absent);
}

void TestModelSelection::catalogGroupsAndFailuresParsed()
{
	const QJsonObject value = parseObject(kCatalogJson);

	const QVector<ModelProviderGroup> groups = ModelSelectionService::parseCatalogGroups(value);
	QCOMPARE(groups.size(), 1);
	QCOMPARE(groups.at(0).id, QStringLiteral("deepseek-official"));
	QCOMPARE(groups.at(0).models.size(), 1);
	QVERIFY(groups.at(0).models.at(0).hasReasoning);
	QCOMPARE(groups.at(0).models.at(0).reasoning.levels.size(), 2);

	const QVector<ModelCatalogFailure> failures =
		ModelSelectionService::parseFailures(value.value(QStringLiteral("failures")).toArray());
	QCOMPARE(failures.size(), 1);
	QCOMPARE(failures.at(0).id, QStringLiteral("broken"));
	QCOMPARE(failures.at(0).name, QStringLiteral("Broken"));
	QCOMPARE(failures.at(0).message, QStringLiteral("boom"));

	// session.models 的 failures 走同一条解析
	const SessionModelDirectory directory =
		ModelSelectionService::parseDirectory(parseObject(kDirectoryJson));
	QCOMPARE(directory.failures.size(), 1);
	// 只给 id 的 failure，name 回退为 id
	QCOMPARE(directory.failures.at(0).name, QStringLiteral("broken"));
}

void TestModelSelection::configuredModelsUseProfilePath()
{
	const QVector<ConfigurableProvider> providers =
		parseProvidersJson(kProvidersJson);
	bool writable = false;
	const QVector<SettingsNamespace> namespaces =
		ModelSelectionService::parseNamespaces(parseObject(kNamespacesJson), &writable);

	// deepseek 系：路径就是 ["models"]
	const ConfigurableProvider& deepseek = providers.at(0);
	QCOMPARE(ModelSelectionService::modelsPath(deepseek), QStringList({ QStringLiteral("models") }));
	const QJsonArray deepseekModels = ModelSelectionService::configuredModels(
		namespaces.at(0), deepseek.settingsPath);
	QCOMPARE(deepseekModels.size(), 1);

	// pi-ai 系：路径是 profile 路径 + "models"
	const ConfigurableProvider& siliconflow = providers.at(1);
	QCOMPARE(ModelSelectionService::modelsPath(siliconflow),
		QStringList({ QStringLiteral("providers"), QStringLiteral("siliconflow-cn"),
			QStringLiteral("models") }));
	QCOMPARE(ModelSelectionService::configuredModels(namespaces.at(1), siliconflow.settingsPath).size(), 2);

	// 该命名空间里不存在的路由 -> 空数组
	const ConfigurableProvider& agnes = providers.at(2);
	QCOMPARE(ModelSelectionService::configuredModels(namespaces.at(1), agnes.settingsPath).size(), 1);

	// 命名空间里完全没有这条路径 -> 空数组而不是崩溃
	QVERIFY(ModelSelectionService::configuredModels(
		namespaces.at(0), { QStringLiteral("providers"), QStringLiteral("x") }).isEmpty());
}

void TestModelSelection::userDeclaresModelsDetectsUserLayerOnly()
{
	const QVector<ConfigurableProvider> providers =
		parseProvidersJson(kProvidersJson);
	bool writable = false;
	const QVector<SettingsNamespace> namespaces =
		ModelSelectionService::parseNamespaces(parseObject(kNamespacesJson), &writable);

	// 用户层写了 models -> true（新增时不会再收窄随附目录，因为已经是显式列表）
	QVERIFY(ModelSelectionService::userDeclaresModels(
		namespaces.at(1), providers.at(1).settingsPath));

	// 用户层没有这条路由 -> false
	QVERIFY(!ModelSelectionService::userDeclaresModels(
		namespaces.at(1), providers.at(2).settingsPath));

	// 整个用户层是空对象 -> false（deepseek 系沿用随附 models）
	QVERIFY(!ModelSelectionService::userDeclaresModels(
		namespaces.at(0), providers.at(0).settingsPath));
}

void TestModelSelection::modelsForWritePrefersUserLayer()
{
	const QVector<ConfigurableProvider> providers =
		parseProvidersJson(kProvidersJson);
	bool writable = false;
	const QVector<SettingsNamespace> namespaces =
		ModelSelectionService::parseNamespaces(parseObject(kNamespacesJson), &writable);

	// 用户层写了 models：以用户层那份为准。settings 的 value 里带着适配器补过的
	// 默认值（input: [] / compat 等），把它写回用户层会让配置越改越胖。
	const QJsonArray siliconWrite = ModelSelectionService::modelsForWrite(
		namespaces.at(1), providers.at(1).settingsPath);
	QCOMPARE(siliconWrite.size(), 2);
	QVERIFY(!siliconWrite.at(0).toObject().contains(QStringLiteral("input")));

	// 用户层没写：退回 value（deepseek 系沿用随附 models），那份带 inputModalities
	const QJsonArray deepseekWrite = ModelSelectionService::modelsForWrite(
		namespaces.at(0), providers.at(0).settingsPath);
	QCOMPARE(deepseekWrite.size(), 1);
	QVERIFY(deepseekWrite.at(0).toObject().contains(QStringLiteral("inputModalities")));

	// 用户层显式写了空数组（= 这个路由不公布任何模型）：不能被随附列表填回来
	SettingsNamespace emptied;
	emptied.user = parseObject(R"({ "providers": { "siliconflow-cn": { "models": [] } } })");
	QVERIFY(ModelSelectionService::userDeclaresModels(
		emptied, providers.at(1).settingsPath));
	QVERIFY(ModelSelectionService::modelsForWrite(emptied, providers.at(1).settingsPath).isEmpty());
}

void TestModelSelection::configuredModelFieldsParsed()
{
	const QVector<ConfigurableProvider> providers =
		parseProvidersJson(kProvidersJson);
	bool writable = false;
	const QVector<SettingsNamespace> namespaces =
		ModelSelectionService::parseNamespaces(parseObject(kNamespacesJson), &writable);

	const QJsonArray models = ModelSelectionService::configuredModels(
		namespaces.at(0), providers.at(0).settingsPath);

	ConfiguredModel flash;
	QVERIFY(ModelSelectionService::parseConfiguredModel(models.at(0), &flash));
	QCOMPARE(flash.id, QStringLiteral("deepseek-v4-flash"));
	QCOMPARE(flash.name, QStringLiteral("DeepSeek-V4-Flash"));
	QVERIFY(flash.hasContextWindow);
	QCOMPARE(flash.contextWindow, 1000000);
	QVERIFY(!flash.hasMaxTokens);
	// inputModalities 这类本表单不解析的字段不进结构体，
	// 它们靠写回时用命名空间里的原始数组带过去（见 modelsForWritePrefersUserLayer）
	QVERIFY(models.at(0).toObject().contains(QStringLiteral("inputModalities")));

	const QJsonArray piAiModels = ModelSelectionService::configuredModels(
		namespaces.at(1), providers.at(1).settingsPath);

	// reasoningEfforts 是字典 -> 收集档位名
	ConfiguredModel silicon;
	QVERIFY(ModelSelectionService::parseConfiguredModel(piAiModels.at(0), &silicon));
	QVERIFY(silicon.hasReasoningEfforts);
	QVERIFY(!silicon.reasoningDisabled);
	QCOMPARE(silicon.reasoningEfforts.size(), 2);
	QVERIFY(silicon.reasoningEfforts.contains(QStringLiteral("high")));

	// reasoningEfforts: false -> 明确不提供档位
	const QJsonArray agnesModels = ModelSelectionService::configuredModels(
		namespaces.at(1), providers.at(2).settingsPath);
	ConfiguredModel agnes;
	QVERIFY(ModelSelectionService::parseConfiguredModel(agnesModels.at(0), &agnes));
	QVERIFY(agnes.hasReasoningEfforts);
	QVERIFY(agnes.reasoningDisabled);

	// 缺 id 的条目被拒绝
	ConfiguredModel invalid;
	QVERIFY(!ModelSelectionService::parseConfiguredModel(
		parseObject(R"({ "name": "no id" })"), &invalid));
}

// ------------------------------------------------------------------
// join：目录 + settings -> 设置面板的行
// ------------------------------------------------------------------

namespace
{
	ServerModelView makeView()
	{
		ServerModelView view;
		view.groups = ModelSelectionService::parseCatalogGroups(parseObject(kCatalogJson));
		view.failures = ModelSelectionService::parseFailures(
			parseObject(kCatalogJson).value(QStringLiteral("failures")).toArray());
		view.providers = parseProvidersJson(kProvidersJson);

		bool writable = false;
		view.namespaces = ModelSelectionService::parseNamespaces(parseObject(kNamespacesJson), &writable);
		view.settingsWritable = writable;
		return view;
	}
}

void TestModelSelection::modelInfosJoinCatalogWithSettings()
{
	const QVector<ModelInfo> rows = ModelSelectionService::buildModelInfos(makeView());

	// 目录 1 条 + settings 里目录没有的 3 条（siliconflow 2 + agnes 1）
	QCOMPARE(rows.size(), 4);

	// 目录里也有、settings 里也声明的模型：档位来自目录，容量来自 settings
	const ModelInfo& flash = rows.at(0);
	QCOMPARE(flash.provider, QStringLiteral("deepseek-official"));
	QCOMPARE(flash.id, QStringLiteral("deepseek-v4-flash"));
	QVERIFY(flash.declared);
	QVERIFY(!flash.userDeclared);      // 声明来自随附配置
	QVERIFY(flash.hasContextWindow);
	QCOMPARE(flash.contextWindow, 1000000);
	QVERIFY(flash.hasReasoning);
	QCOMPARE(flash.levels.size(), 2);
	QCOMPARE(flash.defaultLevelId, QStringLiteral("high"));
	QCOMPARE(flash.settingsNs, QStringLiteral("llm-deepseek"));
	QCOMPARE(flash.settingsPath, QStringList());
	QVERIFY(flash.settingsWritable);

	// pi-ai 里的模型只在 settings 里声明：容量/档位由 settings 提供
	const ModelInfo& silicon = rows.at(1);
	QCOMPARE(silicon.provider, QStringLiteral("siliconflow-cn"));
	QCOMPARE(silicon.id, QStringLiteral("deepseek-ai/DeepSeek-V4-Flash"));
	QVERIFY(silicon.declared);
	QVERIFY(silicon.userDeclared);
	QVERIFY(!silicon.hasContextWindow);
	QVERIFY(silicon.hasReasoning);
	QCOMPARE(silicon.levels.size(), 2);
	QCOMPARE(silicon.settingsPath,
		QStringList({ QStringLiteral("providers"), QStringLiteral("siliconflow-cn") }));

	// reasoningEfforts: false -> 展示层标成“已声明不提供”，且没有档位行
	const ModelInfo& agnes = rows.at(3);
	QCOMPARE(agnes.id, QStringLiteral("agnes-image-2.5-flash"));
	QVERIFY(agnes.reasoningDisabled);
	QVERIFY(!agnes.hasReasoning);
}

void TestModelSelection::modelInfosAppendDeclaredOnlyModels()
{
	// 刚写进 settings、适配器还没重新公布（或该路由此刻加载失败）时，
	// 新模型必须仍然出现在列表里，否则用户会以为没加成功。
	ServerModelView view = makeView();
	view.groups.clear();   // 模拟目录暂时为空

	const QVector<ModelInfo> rows = ModelSelectionService::buildModelInfos(view);

	QCOMPARE(rows.size(), 4);
	QCOMPARE(rows.at(0).id, QStringLiteral("deepseek-v4-flash"));
	QCOMPARE(rows.at(1).id, QStringLiteral("deepseek-ai/DeepSeek-V4-Flash"));
	QCOMPARE(rows.at(2).id, QStringLiteral("Qwen/Qwen-Image"));
	QVERIFY(rows.at(2).hasMaxTokens);
	QCOMPARE(rows.at(2).maxTokens, 8192);
}

void TestModelSelection::modelInfosWithoutSettingsStillListCatalog()
{
	// settings.describe 失败（namespaces 为空）时目录仍要能列出来，
	// 只是没有容量、也没有写回地址。
	ServerModelView view;
	view.groups = ModelSelectionService::parseCatalogGroups(parseObject(kCatalogJson));

	const QVector<ModelInfo> rows = ModelSelectionService::buildModelInfos(view);

	QCOMPARE(rows.size(), 1);
	QVERIFY(!rows.at(0).declared);
	QVERIFY(!rows.at(0).hasContextWindow);
	QVERIFY(rows.at(0).settingsNs.isEmpty());
	QVERIFY(rows.at(0).hasReasoning);
}

// ------------------------------------------------------------------
// 新增模型：条目拼装与整份数组 upsert
// ------------------------------------------------------------------

void TestModelSelection::modelEntryForPiAiCarriesReasoningEfforts()
{
	AddModelRequest request;
	request.provider = QStringLiteral("siliconflow-cn");
	request.id = QStringLiteral("Qwen/Qwen3-Max");
	request.name = QStringLiteral("Qwen3 Max");
	request.hasContextWindow = true;
	request.contextWindow = 262144;
	request.hasMaxTokens = true;
	request.maxTokens = 32768;
	request.reasoningEfforts = { QStringLiteral("off"), QStringLiteral("high") };

	const QJsonObject entry = ModelSelectionService::buildModelEntry(request, true);

	QCOMPARE(entry.value(QStringLiteral("id")).toString(), QStringLiteral("Qwen/Qwen3-Max"));
	QCOMPARE(entry.value(QStringLiteral("name")).toString(), QStringLiteral("Qwen3 Max"));
	QCOMPARE(entry.value(QStringLiteral("contextWindow")).toInt(), 262144);
	QCOMPARE(entry.value(QStringLiteral("maxTokens")).toInt(), 32768);

	const QJsonObject efforts = entry.value(QStringLiteral("reasoningEfforts")).toObject();
	QCOMPARE(efforts.size(), 2);
	QVERIFY(efforts.contains(QStringLiteral("off")));
	QVERIFY(efforts.contains(QStringLiteral("high")));
	// pi-ai 不认 inputModalities，别写进去
	QVERIFY(!entry.contains(QStringLiteral("inputModalities")));
}

void TestModelSelection::modelEntryForDeepSeekCarriesInputModalities()
{
	AddModelRequest request;
	request.provider = QStringLiteral("deepseek-official");
	request.id = QStringLiteral("deepseek-v4-flash-vision-exp");
	request.imageInput = true;

	const QJsonObject entry = ModelSelectionService::buildModelEntry(request, false);

	const QJsonArray modalities = entry.value(QStringLiteral("inputModalities")).toArray();
	QCOMPARE(modalities.size(), 2);
	QCOMPARE(modalities.at(0).toString(), QStringLiteral("text"));
	QCOMPARE(modalities.at(1).toString(), QStringLiteral("image"));
	// deepseek 不认 reasoningEfforts，别写进去
	QVERIFY(!entry.contains(QStringLiteral("reasoningEfforts")));
}

void TestModelSelection::modelEntryOmitsEmptyOptionalFields()
{
	AddModelRequest request;
	request.provider = QStringLiteral("siliconflow-cn");
	request.id = QStringLiteral("  Qwen/Qwen3-Max  ");   // 前后空白要被 trim

	const QJsonObject entry = ModelSelectionService::buildModelEntry(request, true);

	QCOMPARE(entry.value(QStringLiteral("id")).toString(), QStringLiteral("Qwen/Qwen3-Max"));
	// 没填的字段一律不出现：写空字符串/0 会让适配器解析失败
	QVERIFY(!entry.contains(QStringLiteral("name")));
	QVERIFY(!entry.contains(QStringLiteral("contextWindow")));
	QVERIFY(!entry.contains(QStringLiteral("maxTokens")));
	QVERIFY(!entry.contains(QStringLiteral("reasoningEfforts")));
	QVERIFY(!entry.contains(QStringLiteral("inputModalities")));

	// 明确声明“不提供思考档位”是合法的，写成 false
	AddModelRequest noReasoning;
	noReasoning.provider = QStringLiteral("siliconflow-cn");
	noReasoning.id = QStringLiteral("image-only");
	noReasoning.reasoningDisabled = true;
	const QJsonObject disabled = ModelSelectionService::buildModelEntry(noReasoning, true);
	QVERIFY(disabled.value(QStringLiteral("reasoningEfforts")).isBool());
	QVERIFY(!disabled.value(QStringLiteral("reasoningEfforts")).toBool());
}

void TestModelSelection::upsertAppendsNewModel()
{
	const QJsonArray existing = QJsonArray{
		parseObject(R"({ "id": "a", "name": "A" })"),
		parseObject(R"({ "id": "b", "custom": 1 })"),
	};
	const QJsonObject entry = parseObject(R"({ "id": "c" })");

	const QJsonArray merged = ModelSelectionService::upsertModelEntry(existing, entry);

	QCOMPARE(merged.size(), 3);
	QCOMPARE(merged.at(2).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("c"));
	// 已有条目原样保留（含本表单不认识的 custom 字段）
	QCOMPARE(merged.at(1).toObject().value(QStringLiteral("custom")).toInt(), 1);
	// 原数组不被改动
	QCOMPARE(existing.size(), 2);
}

// ------------------------------------------------------------------
// 凭据（API Key）：引用名派生与状态解析
// ------------------------------------------------------------------

void TestModelSelection::keyRefDerivedFromRoute()
{
	// 与 harness 的约定一致：非字母数字换成下划线，再补 _API_KEY
	QCOMPARE(ModelSelectionService::deriveKeyRef(QStringLiteral("siliconflow-cn")),
		QStringLiteral("SILICONFLOW_CN_API_KEY"));
	QCOMPARE(ModelSelectionService::deriveKeyRef(QStringLiteral("deepseek-official")),
		QStringLiteral("DEEPSEEK_OFFICIAL_API_KEY"));
	QCOMPARE(ModelSelectionService::deriveKeyRef(QStringLiteral("zai-coding-cn")),
		QStringLiteral("ZAI_CODING_CN_API_KEY"));

	// 首尾多余分隔符不会留下双下划线/前导下划线（否则不符合引用名的模式）
	QCOMPARE(ModelSelectionService::deriveKeyRef(QStringLiteral("openai-codex")),
		QStringLiteral("OPENAI_CODEX_API_KEY"));
	QCOMPARE(ModelSelectionService::deriveKeyRef(QStringLiteral("-weird-")),
		QStringLiteral("WEIRD_API_KEY"));

	// 无法派生的空路由不给引用名
	QVERIFY(ModelSelectionService::deriveKeyRef(QString()).isEmpty());
	QVERIFY(ModelSelectionService::deriveKeyRef(QStringLiteral("---")).isEmpty());
}

void TestModelSelection::keyRefPrefersProfileApiKeyEnv()
{
	const QVector<ConfigurableProvider> providers =
		parseProvidersJson(kProvidersJson);
	bool writable = false;
	const QVector<SettingsNamespace> namespaces =
		ModelSelectionService::parseNamespaces(parseObject(kNamespacesJson), &writable);

	// 真实回包里 profile 明确点名了 apiKeyEnv，就用它
	QCOMPARE(ModelSelectionService::profileApiKeyEnv(
		namespaces.at(0), providers.at(0).settingsPath), QStringLiteral("DEEPSEEK_API_KEY"));
	QCOMPARE(ModelSelectionService::resolveKeyRef(
		providers.at(0).provider, namespaces.at(0), providers.at(0).settingsPath),
		QStringLiteral("DEEPSEEK_API_KEY"));

	// profile 没点名（测试数据里 pi-ai 的 profile 不带 apiKeyEnv）-> 按约定派生
	QVERIFY(ModelSelectionService::profileApiKeyEnv(
		namespaces.at(1), providers.at(1).settingsPath).isEmpty());
	QCOMPARE(ModelSelectionService::resolveKeyRef(
		providers.at(1).provider, namespaces.at(1), providers.at(1).settingsPath),
		QStringLiteral("SILICONFLOW_CN_API_KEY"));

	// 路径不存在时也退回派生，而不是崩
	QCOMPARE(ModelSelectionService::resolveKeyRef(
		QStringLiteral("agnes"), namespaces.at(1), { QStringLiteral("providers"), QStringLiteral("nope") }),
		QStringLiteral("AGNES_API_KEY"));
}

void TestModelSelection::credentialStatusParsed()
{
	// 取自真实 credentials/describe 回包（0.1.5：以引用名为键的直接映射，没有外层包装）
	const QJsonObject value = parseObject(R"({
		"DEEPSEEK_API_KEY": { "configured": true, "source": "file", "writable": true },
		"ANTHROPIC_API_KEY": { "configured": false, "writable": true },
		"MANAGED_API_KEY": { "configured": true, "source": "env", "writable": false }
	})");

	const CredentialStatus configured =
		ModelSelectionService::parseCredential(QStringLiteral("DEEPSEEK_API_KEY"), value);
	QCOMPARE(configured.ref, QStringLiteral("DEEPSEEK_API_KEY"));
	QVERIFY(configured.configured);
	QVERIFY(configured.writable);
	QCOMPARE(configured.source, QStringLiteral("file"));

	const CredentialStatus missing =
		ModelSelectionService::parseCredential(QStringLiteral("ANTHROPIC_API_KEY"), value);
	QVERIFY(!missing.configured);
	QVERIFY(missing.writable);
	QVERIFY(missing.source.isEmpty());

	// 部署交给环境变量管理：可读不可写，界面据此把输入框禁掉
	const CredentialStatus managed =
		ModelSelectionService::parseCredential(QStringLiteral("MANAGED_API_KEY"), value);
	QVERIFY(managed.configured);
	QVERIFY(!managed.writable);

	// 回包里完全没有的引用 -> known 保持 false（不能据此判定只读），引用名仍带出来
	const CredentialStatus unknown =
		ModelSelectionService::parseCredential(QStringLiteral("NOPE_API_KEY"), value);
	QCOMPARE(unknown.ref, QStringLiteral("NOPE_API_KEY"));
	QVERIFY(!unknown.known);
	QVERIFY(!unknown.configured);
	QVERIFY(!unknown.writable);

	// 服务端回报过的引用一律 known=true（可写与否另看 writable）
	QVERIFY(configured.known);
	QVERIFY(missing.known);
	QVERIFY(managed.known);
}

void TestModelSelection::removeDropsMatchingIdOnly()
{
	const QJsonArray existing = QJsonArray{
		parseObject(R"({ "id": "a", "name": "A" })"),
		parseObject(R"({ "id": "b", "compat": { "x": 1 } })"),
		parseObject(R"({ "id": "c" })"),
	};

	const QJsonArray remaining = ModelSelectionService::removeModelEntry(existing, QStringLiteral("b"));

	QCOMPARE(remaining.size(), 2);
	QCOMPARE(remaining.at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("a"));
	QCOMPARE(remaining.at(1).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("c"));
	// 原数组不被改动（写回用的就是原始那份）
	QCOMPARE(existing.size(), 3);
}

void TestModelSelection::removeIsNoOpForUnknownId()
{
	const QJsonArray existing = QJsonArray{
		parseObject(R"({ "id": "a" })"),
		parseObject(R"({ "id": "b" })"),
	};

	// 删不存在的 id：长度不变（调用方据此判断“没什么可写”）
	QCOMPARE(ModelSelectionService::removeModelEntry(existing, QStringLiteral("nope")).size(), 2);
	// 空 id 同样不动
	QCOMPARE(ModelSelectionService::removeModelEntry(existing, QString()).size(), 2);
	// 空数组不崩
	QVERIFY(ModelSelectionService::removeModelEntry(QJsonArray(), QStringLiteral("a")).isEmpty());
}

void TestModelSelection::upsertReplacesSameIdInPlace()
{
	const QJsonArray existing = QJsonArray{
		parseObject(R"({ "id": "a", "name": "old", "compat": { "x": 1 } })"),
		parseObject(R"({ "id": "b" })"),
	};
	const QJsonObject entry = parseObject(R"({ "id": "a", "name": "new" })");

	const QJsonArray merged = ModelSelectionService::upsertModelEntry(existing, entry);

	// 同 ID 原地替换：位置不变、不追加
	QCOMPARE(merged.size(), 2);
	QCOMPARE(merged.at(0).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("new"));
	QCOMPARE(merged.at(1).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("b"));
}