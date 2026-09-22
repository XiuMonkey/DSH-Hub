#pragma once

// ------------------------------------------------------------------
// ModelSelectionService.h
// ------------------------------------------------------------------
// 会话级“模型 / 思考档位”选择的纯逻辑（不依赖任何 Qt Widget），
// 供输入框底部的模型选择控件与“设置 → 模型列表”面板使用：
//   - 解析 session/modelCatalog 的返回体（部署默认选择、可路由提供方、按提供方分组的
//     模型目录、每个确切模型由适配器公布的思考档位）；
//   - 解析 session.selectModel 的返回体；
//   - 派生查询：当前模型的可用档位、当前档位名；
//   - 解析 llm/listConfigurableProviders（可配置提供方目录）；
//   - 解析 settings.describe（各 settings 命名空间的分层视图）；
//   - 把「目录里的模型」与「settings 里声明的模型条目」join 成一行展示数据；
//   - 通过 settings.mutate 向服务端新增模型条目。
//
// 对应 harness 协议（dsh 0.1.5 的 typert 描述符；端点是 `<namespace>/<method>`，
// 请求体调用方只需给 args 内容，信封由 DshApiClient::callMethod 负责包）：
//   session/modelCatalog  {} -> { default:{provider,model,reasoningEffort?},
//                                 routableProviders:[id],
//                                 groups:[{id,name,models:[{id,name,description?,
//                                          reasoning:{efforts,defaultEffort?}}]}],
//                                 failures:[{id,name,message}] }
//                        注意：catalog 是**部署级**的，default 是部署默认选择；
//                        会话自己的选择在 session/list 行的
//                        projections.values.modelSelection（next → lastUsed）里。
//   session/selectModel   { request:{ sessionId, provider, model, reasoningEffort? } } -> { selected }
//   llm/listConfigurableProviders {} -> [{ provider, displayName, settingsNs,
//                                         settingsPath, declared?, error? }]（**裸数组**）
//   llm/discoverModels    { settingsNs, request:{ provider?, baseURL?, api?, apiKey? } }
//                         -> [{ id, name?, contextWindow?, maxTokens? }]（**裸数组**）
//                         "这条路由能服务哪些模型"由适配器回答：命名一条适配器认得的
//                         路由就用它自己的目录（不联网），否则要 baseURL 才去问端点。
//                         没注册发现的命名空间会以 llm/model-discovery-rejected 失败。
//   settings/describe     {} -> { writable, hasDocument, namespaces: [{ ns, schema,
//                                         value, base?, user?, applies, secrets,
//                                         revision }] }
//   settings/mutate       { ns, ops: [{ op, path, value? }], expectedRevision? } -> 命名空间视图
//   其中 model.reasoning = { efforts: [{ id, name, description? }], defaultEffort? }
// 回包里按提供方统计的 failures（某个提供方目录加载失败）也会被解析，
// 供“模型列表”展示加载失败的来源。
//
// 「省略 reasoningEffort」表示使用提供方默认档位，与显式选中同名档位
// 在请求上等价；界面因此把空值显示为该模型的 defaultEffort。
//
// 模型信息是服务端的责任：模型目录由适配器公布（session/modelCatalog），
// 用户新增的模型写在 settings 文档里（llm-deepseek 整节即 profile，
// 因此 settingsPath 为空；llm-pi-ai 则是 providers.<路由>）。本文件只做
// “读服务端 / 写服务端”的解析与拼装，不持有任何自己的模型清单。
// ------------------------------------------------------------------

#include "network/DshApiClient.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

// 适配器公布的一个思考档位
struct ReasoningLevel
{
	QString id;
	QString name;        // 展示名；适配器没给名字时回退为 id
	QString description;
};

// 某个确切模型的推理元数据
struct ReasoningMetadata
{
	QVector<ReasoningLevel> levels;
	QString defaultLevelId; // 适配器报告的默认档位（可空）
};

// 适配器没有为某个模型公布思考档位时，界面提供的通用兜底档位。
//
// 四档 off/low/high/max：harness 的 THINKING_LEVELS 里最通用的一组，多数路由都认。
// 有了它，未公布档位的模型仍能调档，而不是只剩一句“未公布”。
//
// 返回静态引用：currentLevel() 会返回指向其中元素的指针，地址必须稳定，
// 不能每次现造一个临时 vector。
inline const QVector<ReasoningLevel>& fallbackReasoningLevels()
{
	static const QVector<ReasoningLevel> levels = [] {
		const auto make = [](const char* id, const char* name) {
			ReasoningLevel level;
			level.id = QString::fromLatin1(id);
			level.name = QString::fromLatin1(name);
			return level;
			};

		QVector<ReasoningLevel> built;
		built.reserve(4);
		built.append(make("off", "Off"));
		built.append(make("low", "Low"));
		built.append(make("high", "High"));
		built.append(make("max", "Max"));
		return built;
		}();

	return levels;
}

struct ModelOption
{
	QString id;
	QString name;
	QString description;
	bool hasReasoning = false; // 适配器是否公布了推理元数据
	ReasoningMetadata reasoning;
};

struct ModelProviderGroup
{
	QString id;
	QString name;
	QVector<ModelOption> models;
};

// 某个提供方的目录加载失败（modelCatalog 的 failures）
struct ModelCatalogFailure
{
	QString id;
	QString name;    // 提供方展示名；没给时回退为 id
	QString message;
};

// 完整选择：提供方 + 模型 + 可选思考档位
struct ModelSelection
{
	QString provider;
	QString model;
	QString reasoningEffort;

	bool isValid() const
	{
		return !provider.isEmpty() && !model.isEmpty();
	}
};

// llm.providers 的一行：一个可配置的提供方路由
struct ConfigurableProvider
{
	QString provider;          // 路由 id，也就是 session.selectModel 的 provider
	QString displayName;       // 展示名；服务端没给时回退为 provider
	QString settingsNs;        // 该路由 profile 所在的 settings 命名空间
	QStringList settingsPath;  // 命名空间内到该 route profile 的路径（deepseek 系为空）
	bool active = false;       // 适配器当前是否真的在服务这条路由
	bool declared = false;     // 是否由用户手工声明（适配器随附目录里没有它）
};

// settings.describe 里的一个命名空间视图
struct SettingsNamespace
{
	QString ns;
	QJsonObject value;   // 合并后的生效值（base + 用户层）
	QJsonObject user;    // 用户层（可能为空对象 = 没有用户配置）
	int revision = 0;

	// 按路径取子对象；路径为空时返回自身，找不到（或中间层不存在）返回空对象。
	// value 与 user 两层共用这一份实现，避免各写一遍路径遍历。
	static QJsonObject objectAtPath(const QJsonObject& root, const QStringList& path)
	{
		QJsonObject current = root;
		for (const QString& key : path) {
			current = current.value(key).toObject();
			if (current.isEmpty())
				return QJsonObject();
		}
		return current;
	}

	QJsonObject objectAt(const QStringList& path) const { return objectAtPath(value, path); }
	QJsonObject userAt(const QStringList& path) const { return objectAtPath(user, path); }
};

// settings 里显式声明的一个模型条目。
// 条目之外的未知字段不在结构体里保存：写回时用的是命名空间里那份原始数组
// （见 modelsForWrite），没被解析的字段因此天然不会丢。
struct ConfiguredModel
{
	QString id;
	QString name;                     // 缺省回退为 id
	bool hasContextWindow = false;
	int contextWindow = 0;
	bool hasMaxTokens = false;
	int maxTokens = 0;
	bool hasReasoningEfforts = false;  // 是否写了 reasoningEfforts
	bool reasoningDisabled = false;    // reasoningEfforts: false（明确不提供档位）
	QVector<QString> reasoningEfforts; // reasoningEfforts 的键（off/minimal/low/medium/high/xhigh/max）
};

// llm/discoverModels 回来的一个候选模型：端点自称能服务的模型。
// 除 id 之外都是可选的——多数端点只报 id，容量要用户自己补。
struct DiscoveredModel
{
	QString id;
	QString name;               // 端点给了展示名才有；没有时为空（不必回退成 id）
	bool hasContextWindow = false;
	int contextWindow = 0;
	bool hasMaxTokens = false;
	int maxTokens = 0;
};

// 设置面板里的一行：目录模型 + settings 声明条目 join 之后的结果
struct ModelInfo
{
	QString provider;
	QString providerName;
	QString id;
	QString name;
	QString description;

	bool hasReasoning = false;
	QVector<ReasoningLevel> levels;
	QString defaultLevelId;

	// 以下来自 settings 声明：声明过才可信（目录不公布容量）
	bool declared = false;       // 是否在 settings 的 models 列表里显式声明
	bool userDeclared = false;   // 该声明是否来自用户层（而非随附 base）
	bool hasContextWindow = false;
	int contextWindow = 0;
	bool hasMaxTokens = false;
	int maxTokens = 0;
	bool reasoningDisabled = false;

	QString settingsNs;
	QStringList settingsPath;
	bool settingsWritable = false;
};

// session/modelCatalog 的一次回包
struct SessionModelDirectory
{
	ModelSelection current;
	bool routable = false;
	QVector<ModelProviderGroup> groups;
	QVector<ModelCatalogFailure> failures;

	// 在目录里找某个确切模型；目录不含该模型（适配器仍可服务未公布的模型）时返回 nullptr
	const ModelOption* findModel(const QString& provider, const QString& model) const
	{
		for (const ModelProviderGroup& group : groups) {
			if (group.id != provider)
				continue;
			for (const ModelOption& option : group.models) {
				if (option.id == model)
					return &option;
			}
		}
		return nullptr;
	}

	// 当前选中模型公布的思考档位；没有元数据时为空
	QVector<ReasoningLevel> currentLevels() const
	{
		const ModelOption* option = findModel(current.provider, current.model);
		return option ? option->reasoning.levels : QVector<ReasoningLevel>();
	}

	// 界面真正可以选的档位：适配器公布优先；一个都没公布时用通用兜底四档，
	// 这样“未公布档位”的模型也能调档（见 fallbackReasoningLevels）。
	QVector<ReasoningLevel> selectableLevels() const
	{
		const QVector<ReasoningLevel> published = currentLevels();
		return published.isEmpty() ? fallbackReasoningLevels() : published;
	}

	// 当前档位是否来自通用兜底（界面据此注明“通用档位”，不冒充适配器公布的能力）
	bool usesFallbackLevels() const
	{
		return currentLevels().isEmpty();
	}

	const ReasoningLevel* currentLevel() const
	{
		const ModelOption* option = findModel(current.provider, current.model);
		if (!option)
			return nullptr;

		// 公布为空时改用兜底四档（两者地址都稳定，可以安全取元素指针）
		const QVector<ReasoningLevel>& levels = option->reasoning.levels.isEmpty()
			? fallbackReasoningLevels()
			: option->reasoning.levels;

		const QString wanted = current.reasoningEffort.isEmpty()
			? option->reasoning.defaultLevelId
			: current.reasoningEffort;
		if (wanted.isEmpty())
			return nullptr;

		for (const ReasoningLevel& level : levels) {
			if (level.id == wanted)
				return &level;
		}
		return nullptr;
	}

	// 当前档位的展示名；没有可用档位时为空
	QString currentLevelName() const
	{
		const ReasoningLevel* level = currentLevel();
		return level ? level->name : QString();
	}

	// 当前模型的展示名；目录里没有该模型时回退为 id
	QString currentModelName() const
	{
		const ModelOption* option = findModel(current.provider, current.model);
		return option ? option->name : current.model;
	}
};

// session/modelCatalog + llm/listConfigurableProviders + settings/describe 拼出来的“服务端模型视图”
struct ServerModelView
{
	QVector<ModelProviderGroup> groups;         // session/modelCatalog 的目录
	QVector<ModelCatalogFailure> failures;      // 目录加载失败的提供方
	QVector<ConfigurableProvider> providers;    // llm.providers 的可配置路由
	QVector<SettingsNamespace> namespaces;      // settings.describe 的命名空间视图
	bool settingsWritable = false;              // 设置提供方是否接受写入

	// 找某个路由的可配置目录条目；没有返回 nullptr
	const ConfigurableProvider* findProvider(const QString& provider) const
	{
		for (const ConfigurableProvider& entry : providers) {
			if (entry.provider == provider)
				return &entry;
		}
		return nullptr;
	}

	// 找某个命名空间视图；没有返回 nullptr
	const SettingsNamespace* findNamespace(const QString& ns) const
	{
		for (const SettingsNamespace& entry : namespaces) {
			if (entry.ns == ns)
				return &entry;
		}
		return nullptr;
	}
};

// 写回地址：models.mutate 用
// ------------------------------------------------------------------

// 某个凭据引用在服务端的状态（credentials.describe 的一行）
struct CredentialStatus
{
	QString ref;
	// 服务端是否回报了这个引用。false = 还没查回来，或回包没带它。
	// 界面据此区分“还没问到”与“问到了但只读”——不这样区分就会把未知当只读。
	bool known = false;
	bool configured = false; // 该引用是否已有值
	bool writable = false;   // 该引用当前是否可写（部署可能把它交给环境变量管理）
	QString source;          // 值来自哪里（file / env …）
};

// 新增模型时表单填出来的内容
struct AddModelRequest
{
	QString provider;      // 目标提供方路由（llm.providers 里的一项）
	QString id;            // 模型 id（必填）
	QString name;          // 展示名（可空：服务端/适配器回退为 id）
	bool hasContextWindow = false;
	int contextWindow = 0;
	bool hasMaxTokens = false;
	int maxTokens = 0;

	// llm-pi-ai 的 reasoningEfforts：空 = 不声明（沿用同 id 已安装条目的档位）
	// reasoningDisabled = true 时写成 false，表示该模型明确不提供档位
	bool reasoningDisabled = false;
	QVector<QString> reasoningEfforts;

	// llm-deepseek 的 inputModalities
	bool imageInput = false;

	// 凭据（API Key）随这次新增一起处理：
	//   apiKeyRef  = 该路由 profile 的 apiKeyEnv，没有时用 <路由>_API_KEY 派生；
	//   recordApiKeyEnv = profile 原本没写 apiKeyEnv，需要把派生出来的引用记进去，
	//                     否则这条路由不会去用这把 key。
	QString apiKeyRef;
	bool recordApiKeyEnv = false;

	bool isValid() const { return !provider.isEmpty() && !id.isEmpty(); }
};

namespace ModelSelectionService
{
	inline ModelSelection parseSelection(const QJsonObject& value)
	{
		ModelSelection selection;
		selection.provider = value.value(QStringLiteral("provider")).toString();
		selection.model = value.value(QStringLiteral("model")).toString();
		selection.reasoningEffort = value.value(QStringLiteral("reasoningEffort")).toString();
		return selection;
	}

	inline ReasoningMetadata parseReasoning(const QJsonObject& value)
	{
		ReasoningMetadata metadata;

		const QJsonArray efforts = value.value(QStringLiteral("efforts")).toArray();
		metadata.levels.reserve(efforts.size());
		for (const auto& item : efforts) {
			const QJsonObject effort = item.toObject();

			ReasoningLevel level;
			level.id = effort.value(QStringLiteral("id")).toString();
			if (level.id.isEmpty())
				continue;

			level.name = effort.value(QStringLiteral("name")).toString();
			if (level.name.isEmpty())
				level.name = level.id;
			level.description = effort.value(QStringLiteral("description")).toString();

			metadata.levels.append(level);
		}

		metadata.defaultLevelId = value.value(QStringLiteral("defaultEffort")).toString();
		return metadata;
	}

	// 解析 groups 数组
	inline QVector<ModelProviderGroup> parseGroups(const QJsonArray& groups)
	{
		QVector<ModelProviderGroup> parsed;
		parsed.reserve(groups.size());

		for (const auto& groupValue : groups) {
			const QJsonObject groupObject = groupValue.toObject();

			ModelProviderGroup group;
			group.id = groupObject.value(QStringLiteral("id")).toString();
			if (group.id.isEmpty())
				continue;

			group.name = groupObject.value(QStringLiteral("name")).toString();
			if (group.name.isEmpty())
				group.name = group.id;

			const QJsonArray models = groupObject.value(QStringLiteral("models")).toArray();
			group.models.reserve(models.size());
			for (const auto& modelValue : models) {
				const QJsonObject modelObject = modelValue.toObject();

				ModelOption option;
				option.id = modelObject.value(QStringLiteral("id")).toString();
				if (option.id.isEmpty())
					continue;

				option.name = modelObject.value(QStringLiteral("name")).toString();
				if (option.name.isEmpty())
					option.name = option.id;
				option.description = modelObject.value(QStringLiteral("description")).toString();

				// reasoning 缺席 = 该模型不公布任何思考档位（界面据此隐藏控件）
				if (modelObject.contains(QStringLiteral("reasoning"))) {
					option.reasoning = parseReasoning(modelObject.value(QStringLiteral("reasoning")).toObject());
					option.hasReasoning = !option.reasoning.levels.isEmpty();
				}

				group.models.append(option);
			}

			parsed.append(group);
		}

		return parsed;
	}

	// 解析 failures 数组
	inline QVector<ModelCatalogFailure> parseFailures(const QJsonArray& failures)
	{
		QVector<ModelCatalogFailure> parsed;
		parsed.reserve(failures.size());

		for (const auto& item : failures) {
			const QJsonObject object = item.toObject();

			ModelCatalogFailure failure;
			failure.id = object.value(QStringLiteral("id")).toString();
			if (failure.id.isEmpty())
				continue;

			failure.name = object.value(QStringLiteral("name")).toString();
			if (failure.name.isEmpty())
				failure.name = failure.id;
			failure.message = object.value(QStringLiteral("message")).toString();

			parsed.append(failure);
		}

		return parsed;
	}

	// 解析 session/modelCatalog 的返回体。
	//
	// dsh 0.1.5 的形状（旧 session.models 已被删除，客户端只认这个）：
	//   default             部署默认的模型选择（会话自己的选择在 session/list 投影里）
	//   routableProviders   当前可路由的提供方 id 列表
	//   groups / failures   目录与各提供方的失败原因
	inline SessionModelDirectory parseDirectory(const QJsonObject& value)
	{
		SessionModelDirectory directory;

		directory.current = parseSelection(value.value(QStringLiteral("default")).toObject());

		const QJsonArray routable = value.value(QStringLiteral("routableProviders")).toArray();
		directory.routable = directory.current.provider.isEmpty()
			? !routable.isEmpty()
			: routable.contains(QJsonValue(directory.current.provider));

		directory.groups = parseGroups(value.value(QStringLiteral("groups")).toArray());
		directory.failures = parseFailures(value.value(QStringLiteral("failures")).toArray());

		return directory;
	}

	// 解析 llm/listConfigurableProviders 的返回体（0.1.5 是**裸数组**）
	inline QVector<ModelProviderGroup> parseCatalogGroups(const QJsonObject& value)
	{
		return parseGroups(value.value(QStringLiteral("groups")).toArray());
	}

	// 解析 llm/listConfigurableProviders 的返回体（裸数组，每项一个可配置路由）
	inline QVector<ConfigurableProvider> parseProviders(const QJsonArray& array)
	{
		QVector<ConfigurableProvider> providers;
		providers.reserve(array.size());

		for (const auto& item : array) {
			const QJsonObject object = item.toObject();

			ConfigurableProvider provider;
			provider.provider = object.value(QStringLiteral("provider")).toString();
			if (provider.provider.isEmpty())
				continue;

			provider.displayName = object.value(QStringLiteral("displayName")).toString();
			if (provider.displayName.isEmpty())
				provider.displayName = provider.provider;

			provider.settingsNs = object.value(QStringLiteral("settingsNs")).toString();

			const QJsonArray path = object.value(QStringLiteral("settingsPath")).toArray();
			provider.settingsPath.reserve(path.size());
			for (const auto& segment : path) {
				const QString key = segment.toString();
				if (!key.isEmpty())
					provider.settingsPath.append(key);
			}

			// 0.1.5 的条目没有 active 字段：列出的都是已声明可配置的路由，视为可用
			provider.active = object.contains(QStringLiteral("active"))
				? object.value(QStringLiteral("active")).toBool()
				: true;
			provider.declared = object.value(QStringLiteral("declared")).toBool();

			providers.append(provider);
		}

		return providers;
	}

	// 解析 llm/discoverModels 的返回体（裸数组；服务端已按 id 去重）。
	// id 缺失或非字符串的条目跳过（与服务端同一条规矩：一行的毛病不该毁掉整份清单）。
	inline QVector<DiscoveredModel> parseDiscoveredModels(const QJsonValue& value)
	{
		const QJsonArray array = value.toArray();

		QVector<DiscoveredModel> models;
		models.reserve(array.size());

		for (const auto& item : array) {
			const QJsonObject object = item.toObject();

			DiscoveredModel model;
			model.id = object.value(QStringLiteral("id")).toString();
			if (model.id.isEmpty())
				continue;

			model.name = object.value(QStringLiteral("name")).toString();

			const QJsonValue contextWindow = object.value(QStringLiteral("contextWindow"));
			if (contextWindow.isDouble()) {
				model.hasContextWindow = true;
				model.contextWindow = contextWindow.toInt();
			}

			const QJsonValue maxTokens = object.value(QStringLiteral("maxTokens"));
			if (maxTokens.isDouble()) {
				model.hasMaxTokens = true;
				model.maxTokens = maxTokens.toInt();
			}

			models.append(model);
		}

		return models;
	}

	// 组装 llm/discoverModels 的 request：把表单此刻的样子描述给服务端。
	//
	// 三个字段的来源与理由：
	//   provider  始终带上——命名了路由，适配器就能用它自己认得的目录回答（不联网），
	//             而且能带上该路由存好的凭据与 profile headers；
	//   baseURL/api  只有 profile 里写了才带（随附目录里的路由本来就不需要端点）；
	//   apiKey    只在用户**此刻在表单里填了** key 时才带：服务端对命名路由会自己去取
	//             存好的凭据，这里传空即走那条路；表单里刚敲进去的 key 该赢过它。
	inline QJsonObject buildDiscoveryRequest(
		const ConfigurableProvider& provider,
		const SettingsNamespace* namespaceView,
		const QString& typedApiKey = QString())
	{
		QJsonObject request;

		if (!provider.provider.isEmpty())
			request.insert(QStringLiteral("provider"), provider.provider);

		if (namespaceView) {
			const QJsonObject profile = namespaceView->objectAt(provider.settingsPath);

			const QString baseURL = profile.value(QStringLiteral("baseURL")).toString().trimmed();
			if (!baseURL.isEmpty())
				request.insert(QStringLiteral("baseURL"), baseURL);

			const QString api = profile.value(QStringLiteral("api")).toString().trimmed();
			if (!api.isEmpty())
				request.insert(QStringLiteral("api"), api);
		}

		const QString apiKey = typedApiKey.trimmed();
		if (!apiKey.isEmpty())
			request.insert(QStringLiteral("apiKey"), apiKey);

		return request;
	}

	// 解析 settings.describe 的命名空间数组；writable 通过 outWritable 带出
	inline QVector<SettingsNamespace> parseNamespaces(const QJsonObject& value, bool* outWritable = nullptr)
	{
		if (outWritable)
			*outWritable = value.value(QStringLiteral("writable")).toBool();

		const QJsonArray array = value.value(QStringLiteral("namespaces")).toArray();

		QVector<SettingsNamespace> namespaces;
		namespaces.reserve(array.size());

		for (const auto& item : array) {
			const QJsonObject object = item.toObject();

			SettingsNamespace view;
			view.ns = object.value(QStringLiteral("ns")).toString();
			if (view.ns.isEmpty())
				continue;

			view.value = object.value(QStringLiteral("value")).toObject();
			view.user = object.value(QStringLiteral("user")).toObject();
			view.revision = object.value(QStringLiteral("revision")).toInt();

			namespaces.append(view);
		}

		return namespaces;
	}

	// 模型条目所在的路径：profile 路径 + "models"
	// （llm-deepseek 整节即 profile，所以路径就是 ["models"]）
	inline QStringList modelsPath(const ConfigurableProvider& provider)
	{
		QStringList path = provider.settingsPath;
		path.append(QStringLiteral("models"));
		return path;
	}

	// ------------------------------------------------------------------
	// 凭据（API Key）
	// ------------------------------------------------------------------
	// 服务端把凭据做成“按引用存取”：profile 用 apiKeyEnv 点明这把 key 的引用名，
	// credentials.set 按引用写入，credentials.describe 按引用回报状态。
	// 所以“给某个提供方填一把 key”完全落在服务端能力上，客户端不自己存 key。

	// 由提供方路由派生约定引用名：非字母数字一律换成下划线，再补 _API_KEY。
	// 与 harness 的约定一致（siliconflow-cn -> SILICONFLOW_CN_API_KEY）。
	inline QString deriveKeyRef(const QString& provider)
	{
		QString normalized;
		normalized.reserve(provider.size());
		for (const QChar ch : provider) {
			normalized.append(ch.isLetterOrNumber() ? ch.toUpper() : QLatin1Char('_'));
		}

		while (normalized.startsWith(QLatin1Char('_')))
			normalized.remove(0, 1);
		while (normalized.endsWith(QLatin1Char('_')))
			normalized.chop(1);

		if (normalized.isEmpty())
			return QString();

		return normalized + QStringLiteral("_API_KEY");
	}

	// profile 里写的 apiKeyEnv；没写时返回空串
	inline QString profileApiKeyEnv(const SettingsNamespace& ns, const QStringList& settingsPath)
	{
		return ns.objectAt(settingsPath).value(QStringLiteral("apiKeyEnv")).toString().trimmed();
	}

	// 该路由该用哪个引用：profile 点名的优先，没有就按约定派生
	inline QString resolveKeyRef(
		const QString& provider, const SettingsNamespace& ns, const QStringList& settingsPath)
	{
		const QString declared = profileApiKeyEnv(ns, settingsPath);
		return declared.isEmpty() ? deriveKeyRef(provider) : declared;
	}

	// 解析 credentials/describe 的回包。
	//
	// dsh 0.1.5 的形状是"引用名 -> {configured, source?, writable}"的直接映射。
	// 回包里没有这个引用时 known 保持 false —— 调用方不能把它当成“只读”。
	inline CredentialStatus parseCredential(const QString& ref, const QJsonObject& value)
	{
		CredentialStatus status;
		status.ref = ref;

		const QJsonObject all = value;
		if (!all.contains(ref))
			return status;

		const QJsonObject entry = all.value(ref).toObject();
		status.known = true;
		status.configured = entry.value(QStringLiteral("configured")).toBool();
		status.writable = entry.value(QStringLiteral("writable")).toBool();
		status.source = entry.value(QStringLiteral("source")).toString();

		return status;
	}

	// 把一条模型条目的原始 JSON 解析成可展示字段；id 缺失返回 false
	inline bool parseConfiguredModel(const QJsonValue& value, ConfiguredModel* out)
	{
		if (!out)
			return false;

		const QJsonObject object = value.toObject();

		ConfiguredModel model;
		model.id = object.value(QStringLiteral("id")).toString();
		if (model.id.isEmpty())
			return false;

		model.name = object.value(QStringLiteral("name")).toString();
		if (model.name.isEmpty())
			model.name = model.id;

		const QJsonValue contextWindow = object.value(QStringLiteral("contextWindow"));
		if (contextWindow.isDouble()) {
			model.hasContextWindow = true;
			model.contextWindow = contextWindow.toInt();
		}

		const QJsonValue maxTokens = object.value(QStringLiteral("maxTokens"));
		if (maxTokens.isDouble()) {
			model.hasMaxTokens = true;
			model.maxTokens = maxTokens.toInt();
		}

		// reasoningEfforts 有两种形态：false（明确不提供）或 { 档位: 线上取值 }
		const QJsonValue efforts = object.value(QStringLiteral("reasoningEfforts"));
		if (efforts.isBool()) {
			model.hasReasoningEfforts = true;
			model.reasoningDisabled = !efforts.toBool();
		}
		else if (efforts.isObject()) {
			model.hasReasoningEfforts = true;
			const QJsonObject dict = efforts.toObject();
			for (auto it = dict.constBegin(); it != dict.constEnd(); ++it)
				model.reasoningEfforts.append(it.key());
		}

		*out = model;
		return true;
	}

	// 取命名空间里 profile 路径下的模型数组
	inline QJsonArray configuredModels(const SettingsNamespace& ns, const QStringList& settingsPath)
	{
		return ns.objectAt(settingsPath).value(QStringLiteral("models")).toArray();
	}

	// 该路由的用户层是否自己写了 models 列表。
	// 这决定新增一条的后端语义：数组是整体替换，用户层一旦声明 models，
	// 该路由公布的就只有这一份列表（随附 catalog 不再参与）。
	inline bool userDeclaresModels(const SettingsNamespace& ns, const QStringList& settingsPath)
	{
		return ns.userAt(settingsPath).value(QStringLiteral("models")).isArray();
	}

	// 写回时用的基准数组：优先取用户层自己写的那一份。
	//
	// settings 的 value 是分层合并、并按适配器 schema 补过默认值的结果
	// （真实回包里 siliconflow-cn 的 value 条目带着 input: [] 与
	// compat.chatTemplateKwargs: {}，用户层那一份只有 id）。整份 value 写进用户层
	// 会把解析出来的默认值固化下来，配置会越改越胖；用户层有数组时就以它为准。
	// 用户层没有（首次新增）时才退回 value —— 那份正是当前生效的列表。
	inline QJsonArray modelsForWrite(const SettingsNamespace& ns, const QStringList& settingsPath)
	{
		if (userDeclaresModels(ns, settingsPath)) {
			// 用户层显式写了 models（含空数组）就以它为准：
			// 空数组表示“这个路由不公布任何模型”，写回时不该被随附列表填回来
			return ns.userAt(settingsPath).value(QStringLiteral("models")).toArray();
		}

		return configuredModels(ns, settingsPath);
	}

	// 一条 settings 声明对应的展示行
	inline ModelInfo infoFromConfigured(
		const QString& provider,
		const QString& providerName,
		const ConfiguredModel& model,
		const ConfigurableProvider& entry,
		bool userDeclared,
		bool writable)
	{
		ModelInfo info;
		info.provider = provider;
		info.providerName = providerName;
		info.id = model.id;
		info.name = model.name;
		info.declared = true;
		info.userDeclared = userDeclared;
		info.hasContextWindow = model.hasContextWindow;
		info.contextWindow = model.contextWindow;
		info.hasMaxTokens = model.hasMaxTokens;
		info.maxTokens = model.maxTokens;
		info.reasoningDisabled = model.reasoningDisabled;
		info.settingsNs = entry.settingsNs;
		info.settingsPath = entry.settingsPath;
		info.settingsWritable = writable;

		// settings 只写了档位名；没有适配器公布的展示名与默认档位，
		// 因此把它们保持为空，界面按“只声明了档位”显示。
		for (const QString& level : model.reasoningEfforts) {
			ReasoningLevel reasoning;
			reasoning.id = level;
			reasoning.name = level;
			info.levels.append(reasoning);
		}
		info.hasReasoning = !info.levels.isEmpty();

		return info;
	}

	// 目录模型 -> 展示行（还没有 settings 声明信息时用）
	inline ModelInfo infoFromOption(const QString& provider, const QString& providerName, const ModelOption& option)
	{
		ModelInfo info;
		info.provider = provider;
		info.providerName = providerName;
		info.id = option.id;
		info.name = option.name;
		info.description = option.description;
		info.hasReasoning = option.hasReasoning;
		info.levels = option.reasoning.levels;
		info.defaultLevelId = option.reasoning.defaultLevelId;
		return info;
	}

	// 把目录、可配置提供方目录、settings 命名空间 join 成设置面板要显示的行。
	//
	// 顺序：先按目录的分组顺序输出适配器公布的模型，再补上“只在 settings 里
	// 声明、目录尚未公布”的模型（刚新增、或该路由此刻加载失败时会出现），
	// 这样用户新增完能立刻在列表里看到自己填的那一条。
	inline QVector<ModelInfo> buildModelInfos(const ServerModelView& view)
	{
		QVector<ModelInfo> rows;

		// (provider, modelId) -> rows 下标，用于补声明信息与去重
		QHash<QString, int> index;

		const auto keyOf = [](const QString& provider, const QString& id) {
			return provider + QLatin1Char('\x1f') + id;
			};

		for (const ModelProviderGroup& group : view.groups) {
			for (const ModelOption& option : group.models) {
				const QString key = keyOf(group.id, option.id);
				if (index.contains(key))
					continue;

				index.insert(key, rows.size());
				rows.append(infoFromOption(group.id, group.name, option));
			}
		}

		// 补 settings 声明：容量、是否用户层声明、写回地址都只有这里知道
		for (const ConfigurableProvider& entry : view.providers) {
			const SettingsNamespace* ns = view.findNamespace(entry.settingsNs);
			if (!ns)
				continue;

			const bool writable = view.settingsWritable;
			const bool userDeclared = userDeclaresModels(*ns, entry.settingsPath);

			const QJsonArray models = configuredModels(*ns, entry.settingsPath);
			for (const auto& modelValue : models) {
				ConfiguredModel configured;
				if (!parseConfiguredModel(modelValue, &configured))
					continue;

				const QString key = keyOf(entry.provider, configured.id);
				auto it = index.constFind(key);
				if (it != index.constEnd()) {
					// 目录里已有：补上 settings 侧的容量与写回地址
					ModelInfo& info = rows[it.value()];
					info.declared = true;
					info.userDeclared = userDeclared;
					info.hasContextWindow = configured.hasContextWindow;
					info.contextWindow = configured.contextWindow;
					info.hasMaxTokens = configured.hasMaxTokens;
					info.maxTokens = configured.maxTokens;
					info.reasoningDisabled = configured.reasoningDisabled;
					info.settingsNs = entry.settingsNs;
					info.settingsPath = entry.settingsPath;
					info.settingsWritable = writable;
					continue;
				}

				// 目录里没有：作为“已声明但未公布”的行补进去
				index.insert(key, rows.size());
				rows.append(infoFromConfigured(
					entry.provider, entry.displayName, configured, entry, userDeclared, writable));
			}
		}

		return rows;
	}

	// 拼一条新的模型条目（纯函数，便于单测）。
	// piAi = true 走 llm-pi-ai 的字段（reasoningEfforts / input），
	// 否则走 llm-deepseek 的字段（inputModalities）。
	inline QJsonObject buildModelEntry(const AddModelRequest& request, bool piAi)
	{
		QJsonObject entry;
		entry.insert(QStringLiteral("id"), request.id.trimmed());

		const QString name = request.name.trimmed();
		if (!name.isEmpty())
			entry.insert(QStringLiteral("name"), name);

		if (request.hasContextWindow)
			entry.insert(QStringLiteral("contextWindow"), request.contextWindow);
		if (request.hasMaxTokens)
			entry.insert(QStringLiteral("maxTokens"), request.maxTokens);

		if (piAi) {
			if (request.reasoningDisabled) {
				entry.insert(QStringLiteral("reasoningEfforts"), false);
			}
			else if (!request.reasoningEfforts.isEmpty()) {
				QJsonObject efforts;
				for (const QString& level : request.reasoningEfforts) {
					const QString trimmed = level.trimmed();
					if (!trimmed.isEmpty())
						efforts.insert(trimmed, QJsonValue::Null);
				}
				if (!efforts.isEmpty())
					entry.insert(QStringLiteral("reasoningEfforts"), efforts);
			}
		}
		else {
			QJsonArray modalities;
			modalities.append(QStringLiteral("text"));
			if (request.imageInput)
				modalities.append(QStringLiteral("image"));
			entry.insert(QStringLiteral("inputModalities"), modalities);
		}

		return entry;
	}

	// 把新条目追加到已有数组末尾；同 id 已存在时原地替换（保持原来的位置）。
	// `models` 是数组、写入即整体替换，所以必须是“整份数组 + 新条目”一起写回。
	inline QJsonArray upsertModelEntry(const QJsonArray& existing, const QJsonObject& entry)
	{
		const QString id = entry.value(QStringLiteral("id")).toString();

		QJsonArray result;
		bool replaced = false;

		for (const auto& item : existing) {
			const QJsonObject object = item.toObject();
			if (!replaced && !id.isEmpty() && object.value(QStringLiteral("id")).toString() == id) {
				result.append(entry);
				replaced = true;
				continue;
			}
			result.append(item);
		}

		if (!replaced)
			result.append(entry);

		return result;
	}

	// 从数组里去掉指定 id 的条目。找不到该 id 时原样返回（调用方据此判断“没什么可删”）。
	inline QJsonArray removeModelEntry(const QJsonArray& existing, const QString& modelId)
	{
		if (modelId.isEmpty())
			return existing;

		QJsonArray result;
		for (const auto& item : existing) {
			if (item.toObject().value(QStringLiteral("id")).toString() == modelId)
				continue;
			result.append(item);
		}

		return result;
	}

	// 拉取会话的模型目录（含当前选择与可用思考档位）
	void fetch(
		DshApiClient* api,
		const QString& sessionId,
		const std::function<void(const SessionModelDirectory& directory)>& onLoaded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);

	// 提交完整选择（换思考档位时沿用当前的 provider/model）
	void select(
		DshApiClient* api,
		const QString& sessionId,
		const ModelSelection& selection,
		const std::function<void(const ModelSelection& selected)>& onSelected,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);

	// 拉取“服务端模型视图”：session/modelCatalog + llm/listConfigurableProviders + settings/describe。
	// session/modelCatalog 失败才算整体失败；providers / settings 失败按“暂时读不到”降级，
	// 目录仍然显示（新增模型会因缺写回地址而被禁用）。
	void fetchView(
		DshApiClient* api,
		const std::function<void(const ServerModelView& view)>& onLoaded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);

	// 向某个 settings 命名空间的适配器发现问"这条路由能服务哪些模型"
	// （llm/discoverModels）。request 由 buildDiscoveryRequest 组装。
	// 回包是候选清单，服务端不写任何配置——采纳与否完全在调用方。
	// 该命名空间没注册发现时服务端会以 llm/model-discovery-rejected 失败
	// （例如 deepseek 系路由：它的模型清单只能由 settings 声明）。
	void discoverModels(
		DshApiClient* api,
		const QString& settingsNs,
		const QJsonObject& request,
		const std::function<void(const QVector<DiscoveredModel>& models)>& onLoaded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);

	// 向 settings 追加一个模型条目（读-改-写整份 models 数组）。
	// 若 request.apiKeyRef 非空且 recordApiKeyEnv 为真，会在同一次 mutate 里
	// 额外写入 profile 的 apiKeyEnv，使这条路由真的去用那把 key。
	// onAdded 带回写回后的命名空间视图（可直接用来刷新展示）。
	void addModel(
		DshApiClient* api,
		const ConfigurableProvider& provider,
		const SettingsNamespace& namespaceView,
		const AddModelRequest& request,
		const std::function<void(const SettingsNamespace& updated)>& onAdded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);

	// 向 settings 删掉一个模型条目。与新增同样走“读-改-写整份 models 数组”
	// （数组写入即整体替换，没有按元素删除的写法）。
	// 目标 id 本来就不在这份列表里时，不写服务端，直接把当前视图回报给 onRemoved。
	void removeModel(
		DshApiClient* api,
		const ConfigurableProvider& provider,
		const SettingsNamespace& namespaceView,
		const QString& modelId,
		const std::function<void(const SettingsNamespace& updated)>& onRemoved,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);

	// 查询某个凭据引用的状态（是否已配置、是否可写、值来自哪里）
	void describeCredential(
		DshApiClient* api,
		const QString& ref,
		const std::function<void(const CredentialStatus& status)>& onDescribed,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);

	// 写入某个凭据引用（只写服务端，不回显值）
	void saveCredential(
		DshApiClient* api,
		const QString& ref,
		const QString& value,
		const std::function<void()>& onSaved,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);
}
