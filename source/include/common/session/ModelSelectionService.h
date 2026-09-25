#pragma once

// 会话级“模型 / 思考档位”选择的纯逻辑（无 Qt Widget 依赖）：解析服务端回包，把目录模型与
// settings 声明的条目 join 成展示行，并经 settings/mutate 写回新增模型。
// 协议：端点是 <namespace>/<method>，请求体只给 args；catalog 是部署级的，会话自己的选择在
// session/list 行的投影里；providers 与 discoverModels 返回裸数组。

#include "network/DshApiClient.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

struct ReasoningLevel
{
	QString id;
	QString name; // 缺省回退为 id
	QString description;
};

struct ReasoningMetadata
{
	QVector<ReasoningLevel> levels;
	QString defaultLevelId; // 默认档位，可空
};

// ⚠️ 兜底四档（off/low/high/max）返回静态引用：currentLevel() 的指针地址必须稳定
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
	bool hasReasoning = false;
	ReasoningMetadata reasoning;
};

struct ModelProviderGroup
{
	QString id;
	QString name;
	QVector<ModelOption> models;
};

struct ModelCatalogFailure
{
	QString id;
	QString name; // 缺省回退为 id
	QString message;
};

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

struct ConfigurableProvider
{
	QString provider; // 路由 id，即 selectModel 的 provider
	QString displayName; // 缺省回退为 provider
	QString settingsNs;
	QStringList settingsPath; // 到 route profile 的路径，deepseek 系为空
	bool active = false;
	bool declared = false;
};

struct SettingsNamespace
{
	QString ns;
	QJsonObject value; // 合并后的生效值
	QJsonObject user;
	int revision = 0;

	// 路径为空返回自身，找不到返回空对象
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

// settings 里显式声明的模型条目；写回用原始数组，未解析字段不会丢
struct ConfiguredModel
{
	QString id;
	QString name; // 缺省回退为 id
	bool hasContextWindow = false;
	int contextWindow = 0;
	bool hasMaxTokens = false;
	int maxTokens = 0;
	bool hasReasoningEfforts = false;
	bool reasoningDisabled = false;
	QVector<QString> reasoningEfforts;
};

// discoverModels 的候选模型；除 id 外都可选
struct DiscoveredModel
{
	QString id;
	QString name;
	bool hasContextWindow = false;
	int contextWindow = 0;
	bool hasMaxTokens = false;
	int maxTokens = 0;
};

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

	// 以下来自 settings 声明（目录不公布容量）
	bool declared = false;
	bool userDeclared = false;
	bool hasContextWindow = false;
	int contextWindow = 0;
	bool hasMaxTokens = false;
	int maxTokens = 0;
	bool reasoningDisabled = false;

	QString settingsNs;
	QStringList settingsPath;
	bool settingsWritable = false;
};

struct SessionModelDirectory
{
	ModelSelection current;
	bool routable = false;
	QVector<ModelProviderGroup> groups;
	QVector<ModelCatalogFailure> failures;

	// 目录不含该模型时返回 nullptr
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
	// 当前选中模型公布的档位
	QVector<ReasoningLevel> currentLevels() const
	{
		const ModelOption* option = findModel(current.provider, current.model);
		return option ? option->reasoning.levels : QVector<ReasoningLevel>();
	}
	// 适配器公布优先，都没有时用兜底四档
	QVector<ReasoningLevel> selectableLevels() const
	{
		const QVector<ReasoningLevel> published = currentLevels();
		return published.isEmpty() ? fallbackReasoningLevels() : published;
	}
	// 是否来自兜底（界面据此注明“通用档位”）
	bool usesFallbackLevels() const
	{
		return currentLevels().isEmpty();
	}
	const ReasoningLevel* currentLevel() const
	{
		const ModelOption* option = findModel(current.provider, current.model);
		if (!option)
			return nullptr;

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
	QString currentLevelName() const
	{
		const ReasoningLevel* level = currentLevel();
		return level ? level->name : QString();
	}
	// 目录里没有时回退为 id
	QString currentModelName() const
	{
		const ModelOption* option = findModel(current.provider, current.model);
		return option ? option->name : current.model;
	}
};

// modelCatalog + listConfigurableProviders + settings.describe 拼出的服务端模型视图
struct ServerModelView
{
	QVector<ModelProviderGroup> groups;
	QVector<ModelCatalogFailure> failures;
	QVector<ConfigurableProvider> providers;
	QVector<SettingsNamespace> namespaces;
	bool settingsWritable = false;

	const ConfigurableProvider* findProvider(const QString& provider) const
	{
		for (const ConfigurableProvider& entry : providers) {
			if (entry.provider == provider)
				return &entry;
		}
		return nullptr;
	}
	const SettingsNamespace* findNamespace(const QString& ns) const
	{
		for (const SettingsNamespace& entry : namespaces) {
			if (entry.ns == ns)
				return &entry;
		}
		return nullptr;
	}
};

struct CredentialStatus
{
	QString ref;
	// ⚠️ false 时不能当成只读（可能只是还没查到）
	bool known = false;
	bool configured = false;
	bool writable = false;
	QString source; // 值的来源（file / env …）
};

struct AddModelRequest
{
	QString provider;
	QString id; // 必填
	QString name;
	bool hasContextWindow = false;
	int contextWindow = 0;
	bool hasMaxTokens = false;
	int maxTokens = 0;

	// pi-ai：空 = 不声明；reasoningDisabled 写成 false
	bool reasoningDisabled = false;
	QVector<QString> reasoningEfforts;

	bool imageInput = false;

	// apiKeyRef = profile 的 apiKeyEnv，缺省派生；recordApiKeyEnv 为真时要把派生引用记进 profile
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

				// reasoning 缺席 = 不公布任何档位
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

	// default = 部署默认选择；另有 routableProviders、groups 与 failures
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

	inline QVector<ModelProviderGroup> parseCatalogGroups(const QJsonObject& value)
	{
		return parseGroups(value.value(QStringLiteral("groups")).toArray());
	}

	// 裸数组
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

			// 无 active 字段时视为可用
			provider.active = object.contains(QStringLiteral("active"))
				? object.value(QStringLiteral("active")).toBool()
				: true;
			provider.declared = object.value(QStringLiteral("declared")).toBool();
			providers.append(provider);
		}
		return providers;
	}

	// 裸数组；id 缺失的条目跳过
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

	// baseURL/api 只在 profile 写了才带；apiKey 只在表单刚填了才带
	inline QJsonObject buildDiscoveryRequest(const ConfigurableProvider& provider,
		const SettingsNamespace* namespaceView, const QString& typedApiKey = QString())
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

	// 解析 settings.describe 的命名空间数组
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

	// profile 路径 + "models"
	inline QStringList modelsPath(const ConfigurableProvider& provider)
	{
		QStringList path = provider.settingsPath;
		path.append(QStringLiteral("models"));
		return path;
	}

	// 凭据按引用存取；客户端不自己存 key

	// 非字母数字换下划线、去首尾下划线、补 _API_KEY
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

	// profile 的 apiKeyEnv
	inline QString profileApiKeyEnv(const SettingsNamespace& ns, const QStringList& settingsPath)
	{
		return ns.objectAt(settingsPath).value(QStringLiteral("apiKeyEnv")).toString().trimmed();
	}

	inline QString resolveKeyRef(const QString& provider, const SettingsNamespace& ns, const QStringList& settingsPath)
	{
		const QString declared = profileApiKeyEnv(ns, settingsPath);
		return declared.isEmpty() ? deriveKeyRef(provider) : declared;
	}

	// 回包里没有该引用时 known 保持 false，不能当成「只读」
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

	// id 缺失返回 false
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

		// reasoningEfforts 有两种形态：false 或 { 档位: 取值 }
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

	inline QJsonArray configuredModels(const SettingsNamespace& ns, const QStringList& settingsPath)
	{
		return ns.objectAt(settingsPath).value(QStringLiteral("models")).toArray();
	}

	// 用户层是否自己写了 models；数组是整体替换，声明了就只有这一份
	inline bool userDeclaresModels(const SettingsNamespace& ns, const QStringList& settingsPath)
	{
		return ns.userAt(settingsPath).value(QStringLiteral("models")).isArray();
	}

	// 优先用户层那份 —— 整份 value 写回会把 schema 补的默认值固化；用户层没有时才退回 value
	inline QJsonArray modelsForWrite(const SettingsNamespace& ns, const QStringList& settingsPath)
	{
		if (userDeclaresModels(ns, settingsPath)) {
			return ns.userAt(settingsPath).value(QStringLiteral("models")).toArray();
		}

		return configuredModels(ns, settingsPath);
	}

	inline ModelInfo infoFromConfigured(const QString& provider, const QString& providerName,
		const ConfiguredModel& model, const ConfigurableProvider& entry, bool userDeclared, bool writable)
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

		// settings 只写档位名，没展示名与默认档位，保持为空
		for (const QString& level : model.reasoningEfforts) {
			ReasoningLevel reasoning;
			reasoning.id = level;
			reasoning.name = level;
			info.levels.append(reasoning);
		}
		info.hasReasoning = !info.levels.isEmpty();
		return info;
	}

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

	// join 成面板行：先目录公布的模型，再补只在 settings 声明、目录未公布的
	inline QVector<ModelInfo> buildModelInfos(const ServerModelView& view)
	{
		QVector<ModelInfo> rows;

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

		// 补 settings 声明：容量、写回地址只有这里知道
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
					// 目录里已有：补 settings 侧的容量与写回地址
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

				index.insert(key, rows.size());
				rows.append(infoFromConfigured(entry.provider, entry.displayName, configured,
					entry, userDeclared, writable));
			}
		}
		return rows;
	}

	// piAi 走 reasoningEfforts / input，否则走 inputModalities
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

	// 同 id 原地替换；`models` 写入即整体替换，必须整份写回
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

	// 找不到时原样返回
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

	void fetch(DshApiClient* api, const QString& sessionId,
		const std::function<void(const SessionModelDirectory& directory)>& onLoaded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);
	// 省略 reasoningEffort = 用提供方默认档位
	void select(DshApiClient* api, const QString& sessionId, const ModelSelection& selection,
		const std::function<void(const ModelSelection& selected)>& onSelected,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);
	// 只有 modelCatalog 失败才算整体失败，其余降级为“暂时读不到”
	void fetchView(DshApiClient* api, const std::function<void(const ServerModelView& view)>& onLoaded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);
	// 回包只是候选清单，服务端不写配置；未注册发现时以 llm/model-discovery-rejected 失败
	void discoverModels(DshApiClient* api, const QString& settingsNs, const QJsonObject& request,
		const std::function<void(const QVector<DiscoveredModel>& models)>& onLoaded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);
	// 读-改-写整份 models 数组；apiKeyRef 非空且 recordApiKeyEnv 为真时同一次 mutate 里写入 apiKeyEnv
	void addModel(DshApiClient* api, const ConfigurableProvider& provider,
		const SettingsNamespace& namespaceView, const AddModelRequest& request,
		const std::function<void(const SettingsNamespace& updated)>& onAdded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);
	// 读-改-写整份数组；id 不在列表里时不写服务端，直接回报
	void removeModel(DshApiClient* api, const ConfigurableProvider& provider,
		const SettingsNamespace& namespaceView, const QString& modelId,
		const std::function<void(const SettingsNamespace& updated)>& onRemoved,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);
	void describeCredential(DshApiClient* api, const QString& ref,
		const std::function<void(const CredentialStatus& status)>& onDescribed,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);
	void saveCredential(DshApiClient* api, const QString& ref, const QString& value,
		const std::function<void()>& onSaved,
		const std::function<void(const DshApiClient::RpcError& error)>& onError);
}
