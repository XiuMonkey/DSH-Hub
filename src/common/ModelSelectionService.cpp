#include "ModelSelectionService.h"

#include "SessionCommands.h"

#include <QDebug>
#include <QCoreApplication>

#include <memory>

// ------------------------------------------------------------------
// ModelSelectionService.cpp
// ------------------------------------------------------------------
// 这里只放需要 DshApiClient 的联网部分；解析与派生查询是 inline 在头文件
// 里的（见 ModelSelectionService.h），便于脱离网络做单元测试。
//
// 模型信息的事实来源全在服务端：
//   - 目录（有哪些模型、每个模型公布哪些思考档位）来自适配器：session/modelCatalog
//     （会话级的当前选择在 session/list 的投影里）；
//   - 用户新增/覆盖的模型条目写进 settings 文档：llm-deepseek 整节即 profile，
//     llm-pi-ai 是 providers.<路由>，两者都用 `models` 数组承载条目；
//   - llm/listConfigurableProviders 告诉客户端每条路由的 settings 命名空间与路径，
//     所以客户端不需要把提供方→命名空间的映射写死在自己这里。
// ------------------------------------------------------------------

namespace
{
	// dsh 0.1.5：斜杠 endpoint；无参端点（catalog/providers/describe）args 为空。
	// session.models / llm.models 两个旧端点都并入 session/modelCatalog
	// （返回 { default, routableProviders, groups, failures }）。
	const char* const kModelsMethod = "session/modelCatalog";
	const char* const kSelectModelMethod = "session/selectModel";
	const char* const kLlmModelsMethod = "session/modelCatalog";
	const char* const kLlmProvidersMethod = "llm/listConfigurableProviders";
	const char* const kSettingsDescribeMethod = "settings/describe";
	const char* const kSettingsMutateMethod = "settings/mutate";
	const char* const kCredentialsDescribeMethod = "credentials/describe";
	const char* const kCredentialsSetMethod = "credentials/set";

	// settings/mutate 的一个 set 操作
	QJsonObject setOp(const QStringList& path, const QJsonValue& value)
	{
		QJsonObject op;
		op.insert(QStringLiteral("op"), QStringLiteral("set"));
		op.insert(QStringLiteral("path"), QJsonArray::fromStringList(path));
		op.insert(QStringLiteral("value"), value);
		return op;
	}
}

namespace ModelSelectionService
{
	void fetch(
		DshApiClient* api,
		const QString& sessionId,
		const std::function<void(const SessionModelDirectory& directory)>& onLoaded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError)
	{
		if (!api || sessionId.isEmpty())
			return;

		// dsh 0.1.5：session/models 变成无参的 session/modelCatalog（全局目录），
		// 所以这里不再发 sessionId；会话级的"当前选择"在 session/list 行的
		// projections.values.modelSelection 里（step2 再接）。
		Q_UNUSED(sessionId);

		api->callMethod(
			QLatin1String(kModelsMethod),
			SessionCommands::emptyArgs(),
			[onLoaded](const QJsonObject& value) {
				if (onLoaded)
					onLoaded(parseDirectory(value));
			},
			[onError](const DshApiClient::RpcError& error) {
				if (onError)
					onError(error);
			});
	}

	void select(
		DshApiClient* api,
		const QString& sessionId,
		const ModelSelection& selection,
		const std::function<void(const ModelSelection& selected)>& onSelected,
		const std::function<void(const DshApiClient::RpcError& error)>& onError)
	{
		if (!api || sessionId.isEmpty() || !selection.isValid())
			return;

		api->callMethod(
			QLatin1String(kSelectModelMethod),
			SessionCommands::sessionSelectModel(sessionId, selection.provider, selection.model,
				selection.reasoningEffort),
			[selection, onSelected](const QJsonObject& value) {
				if (!onSelected)
					return;

				// 以服务端回显为准；缺失字段回退到请求值
				ModelSelection selected = parseSelection(value.value(QStringLiteral("selected")).toObject());
				if (selected.provider.isEmpty())
					selected.provider = selection.provider;
				if (selected.model.isEmpty())
					selected.model = selection.model;
				onSelected(selected);
			},
			[onError](const DshApiClient::RpcError& error) {
				if (onError)
					onError(error);
			});
	}

	// ------------------------------------------------------------------
	// 服务端模型视图：目录 + 可配置提供方 + settings 命名空间
	// ------------------------------------------------------------------
	// 三个 RPC 依次发，任一可选部分失败都只降级、不整体失败：
	//   - session/modelCatalog 失败 -> 没有目录可显示，整体失败；
	//   - llm/listConfigurableProviders 失败 -> 目录仍显示，但“新增模型”拿不到写回地址；
	//   - settings/describe 失败 -> 目录仍显示，条目看不到容量/来源，
	//     写回也会被禁用（面板据此提示）。
	// 三个请求的返回顺序不确定：共享状态累积结果，等三个都有着落、
	// 且目录到手之后才回调一次，避免界面为了半份数据重建多次。
	void fetchView(
		DshApiClient* api,
		const std::function<void(const ServerModelView& view)>& onLoaded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError)
	{
		if (!api)
			return;

		struct Accumulator
		{
			ServerModelView view;
			int pending = 3;      // 还有几个 RPC 没回来
			bool catalogDone = false;
			bool delivered = false;

			// 一个 RPC 有了着落；三个都回来且目录到手之后回调一次
			void settle(const std::function<void(const ServerModelView&)>& onLoaded)
			{
				--pending;
				if (delivered || pending > 0 || !catalogDone)
					return;

				delivered = true;
				if (onLoaded)
					onLoaded(view);
			}
		};

		auto shared = std::make_shared<Accumulator>();
		// 三个回调都收敛到这一份：省掉每处重复写条件与空函数
		auto settle = [shared, onLoaded]() { shared->settle(onLoaded); };

		api->callMethod(
			QLatin1String(kLlmModelsMethod),
			SessionCommands::emptyArgs(),
			[shared, settle](const QJsonObject& value) {
				shared->view.groups = parseCatalogGroups(value);
				shared->view.failures = parseFailures(value.value(QStringLiteral("failures")).toArray());
				shared->catalogDone = true;
				settle();
			},
			[settle, onError](const DshApiClient::RpcError& error) {
				settle();
				if (onError)
					onError(error);
			});

		// 0.1.5：llm/models 没了，可选提供方路由用 llm/listConfigurableProviders，
		// 它返回的是**裸数组** [{ provider, displayName, settingsNs, settingsPath, … }]，
		// 所以走 callMethodValue。
		api->callMethodValue(
			QLatin1String(kLlmProvidersMethod),
			SessionCommands::emptyArgs(),
			[shared, settle](const QJsonValue& value) {
				shared->view.providers = parseProviders(value.toArray());
				settle();
			},
			[settle](const DshApiClient::RpcError& error) {
				settle();
				qWarning().noquote() << QStringLiteral("[ModelSelection] llm/listConfigurableProviders failed:")
					<< error.code << error.message;
			});

		api->callMethod(
			QLatin1String(kSettingsDescribeMethod),
			SessionCommands::emptyArgs(),
			[shared, settle](const QJsonObject& value) {
				bool writable = false;
				shared->view.namespaces = parseNamespaces(value, &writable);
				shared->view.settingsWritable = writable;
				settle();
			},
			[settle](const DshApiClient::RpcError& error) {
				settle();
				qWarning().noquote() << QStringLiteral("[ModelSelection] settings/describe failed:")
					<< error.code << error.message;
			});
	}

	// ------------------------------------------------------------------
	// settings/mutate：新增 / 删除模型条目
	// ------------------------------------------------------------------

	// settings/mutate 的公共收尾：回包本身就是该命名空间的新视图，
	// 缺失时用请求前的视图兜底（新增与删除共用）。
	void reportNamespaceUpdate(
		const QJsonObject& value,
		const SettingsNamespace& fallback,
		const std::function<void(const SettingsNamespace& updated)>& onUpdated)
	{
		if (!onUpdated)
			return;

		SettingsNamespace updated = fallback;
		if (value.contains(QStringLiteral("ns"))) {
			const QVector<SettingsNamespace> parsed = parseNamespaces(
				QJsonObject{ { QStringLiteral("namespaces"), QJsonArray{ value } } });
			if (!parsed.isEmpty())
				updated = parsed.first();
		}
		onUpdated(updated);
	}

	// settings/mutate 的公共发送
	void mutateSettings(
		DshApiClient* api,
		const QString& ns,
		const QJsonArray& ops,
		const SettingsNamespace& fallbackView,
		const std::function<void(const SettingsNamespace& updated)>& onUpdated,
		const std::function<void(const DshApiClient::RpcError& error)>& onError)
	{
		api->callMethod(
			QLatin1String(kSettingsMutateMethod),
			SessionCommands::settingsMutate(ns, ops),
			[fallbackView, onUpdated](const QJsonObject& value) {
				reportNamespaceUpdate(value, fallbackView, onUpdated);
			},
			[onError](const DshApiClient::RpcError& error) {
				if (onError)
					onError(error);
			});
	}

	void addModel(
		DshApiClient* api,
		const ConfigurableProvider& provider,
		const SettingsNamespace& namespaceView,
		const AddModelRequest& request,
		const std::function<void(const SettingsNamespace& updated)>& onAdded,
		const std::function<void(const DshApiClient::RpcError& error)>& onError)
	{
		if (!api || !request.isValid() || provider.settingsNs.isEmpty()) {
			if (onError) {
				DshApiClient::RpcError error;
				error.code = QStringLiteral("invalid-request");
				error.message = qtTrId("model_missing_provider_or_id");
				onError(error);
			}
			return;
		}

		// pi-ai 的条目字段与 deepseek 系不同：reasoningEfforts 只属于 pi-ai，
		// inputModalities 只属于 deepseek。命名空间名就是适配器族的标识。
		const bool piAi = provider.settingsNs.contains(QStringLiteral("pi-ai"));

		// `models` 是数组、写入即整体替换，所以必须把“整份列表 + 新条目”一起写回。
		// 基准取用户层已有数组（有的话），避免把 settings 解析出来的默认值固化进用户配置。
		const QJsonArray existing = modelsForWrite(namespaceView, provider.settingsPath);
		const QJsonObject entry = buildModelEntry(request, piAi);
		const QJsonArray merged = upsertModelEntry(existing, entry);

		QJsonArray ops;

		// 表单里填了 key 且这条 profile 原本没点名引用：把引用一并记下来。
		// 少了这一步，新模型所在的整条路由不会去读那把 key（profile 里没有 apiKeyEnv，
		// 适配器会退回环境发现或直接报缺凭据）。
		if (!request.apiKeyRef.isEmpty() && request.recordApiKeyEnv) {
			QStringList path = provider.settingsPath;
			path.append(QStringLiteral("apiKeyEnv"));
			ops.append(setOp(path, request.apiKeyRef));
		}

		ops.append(setOp(modelsPath(provider), merged));

		mutateSettings(api, provider.settingsNs, ops, namespaceView, onAdded, onError);
	}

	// 删除模型：把“去掉该条目后的整份数组”写回去（数组写入即整体替换）
	void removeModel(
		DshApiClient* api,
		const ConfigurableProvider& provider,
		const SettingsNamespace& namespaceView,
		const QString& modelId,
		const std::function<void(const SettingsNamespace& updated)>& onRemoved,
		const std::function<void(const DshApiClient::RpcError& error)>& onError)
	{
		if (!api || modelId.isEmpty() || provider.settingsNs.isEmpty()) {
			if (onError) {
				DshApiClient::RpcError error;
				error.code = QStringLiteral("invalid-request");
				error.message = qtTrId("model_missing_provider_or_id");
				onError(error);
			}
			return;
		}

		const QJsonArray existing = modelsForWrite(namespaceView, provider.settingsPath);
		const QJsonArray remaining = removeModelEntry(existing, modelId);

		// 目标本来就不在这份列表里：没什么可写，直接把当前视图回报给调用方
		if (remaining.size() == existing.size()) {
			if (onRemoved)
				onRemoved(namespaceView);
			return;
		}

		QJsonArray ops;
		ops.append(setOp(modelsPath(provider), remaining));

		mutateSettings(api, provider.settingsNs, ops, namespaceView, onRemoved, onError);
	}

	// ------------------------------------------------------------------
	// 凭据：按引用查询 / 写入
	// ------------------------------------------------------------------
	void describeCredential(
		DshApiClient* api,
		const QString& ref,
		const std::function<void(const CredentialStatus& status)>& onDescribed,
		const std::function<void(const DshApiClient::RpcError& error)>& onError)
	{
		if (!api || ref.isEmpty())
			return;

		api->callMethod(
			QLatin1String(kCredentialsDescribeMethod),
			SessionCommands::credentialsDescribe(QJsonArray{ ref }),
			[ref, onDescribed](const QJsonObject& value) {
				if (onDescribed)
					onDescribed(parseCredential(ref, value));
			},
			[onError](const DshApiClient::RpcError& error) {
				if (onError)
					onError(error);
			});
	}

	void saveCredential(
		DshApiClient* api,
		const QString& ref,
		const QString& value,
		const std::function<void()>& onSaved,
		const std::function<void(const DshApiClient::RpcError& error)>& onError)
	{
		if (!api || ref.isEmpty() || value.isEmpty()) {
			if (onError) {
				DshApiClient::RpcError error;
				error.code = QStringLiteral("invalid-request");
				error.message = qtTrId("model_credential_missing");
				onError(error);
			}
			return;
		}

		// 只发不收：值不回显，回调里也不留任何副本。
		// 0.1.5：credentials/set 的结果是 void，回包里没有 value 字段（正常）。
		api->callMethod(
			QLatin1String(kCredentialsSetMethod),
			SessionCommands::credentialsSet(ref, value),
			[onSaved](const QJsonObject&) {
				if (onSaved)
					onSaved();
			},
			[onError](const DshApiClient::RpcError& error) {
				if (onError)
					onError(error);
			});
	}
}