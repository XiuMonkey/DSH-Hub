#pragma once

// 「设置 → 模型列表」面板：列出服务端公布的模型（可展开表头 + 详情），底部“添加模型”就地拉出填写表单。
// 表单里“获取模型”按当前路由问一次 llm/discoverModels，候选铺成下拉回填模型 ID（只读，不写配置）。
// 表单放在滚动区内部而非窗口底部，面板高度才不随表单开合变化（按钮不会上下跳）；数据与写入都在服务端，本面板不缓存清单。

#include "common/session/ModelSelectionService.h"

#include <QSet>
#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QContextMenuEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class DshApiClient;

// 列表里的一个成员：表头（模型名 + 提供方/模型 id）点击展开/收起详情
class ModelListEntry : public QWidget
{
	Q_OBJECT

public:
	explicit ModelListEntry(const ModelInfo& info, QWidget* parent = nullptr);

	// (provider, modelId) 唯一键：刷新后用来恢复展开状态
	QString key() const;

	bool isExpanded() const;
	void setExpanded(bool expanded);

signals:
	void expandedChanged(const QString& key, bool expanded);

	// 在成员上右键（表头或详情都算）—— 面板据此弹出菜单
	void contextMenuRequested(const QString& provider, const QString& modelId, const QPoint& globalPos);

protected:
	void contextMenuEvent(QContextMenuEvent* event) override;

private:
	// 详情区的一行“名称：值”；值缺失时整行不建
	static void addDetailRow(QVBoxLayout* layout, QWidget* parent,
		const QString& name, const QString& value);

	QString m_key;
	QString m_provider;
	QString m_modelId;
	QPushButton* m_header = nullptr;
	QLabel* m_chevron = nullptr;
	QWidget* m_detail = nullptr;
};

class ModelListPanel : public QWidget
{
	Q_OBJECT

public:
	explicit ModelListPanel(DshApiClient* api, QWidget* parent = nullptr);

	// 重新向服务端拉取目录/提供方/命名空间并重建列表
	void refresh();

signals:
	void modelAdded(const QString& provider, const QString& modelId);

private:
	void applyView(const ServerModelView& view);
	void clearRows();
	void populateRows();
	void updateStatus();

	// 添加模型的表单
	void buildForm(QWidget* parent);
	void openForm();
	void closeForm();
	void syncFormToRoute();
	void submitForm();
	// 表单当前选择的提供方路由；没有可选项时返回 nullptr
	const ConfigurableProvider* selectedProvider() const;
	// 该路由此刻在命名空间里的状态说明（含“写整份数组会替换随附目录”的提醒）
	QString routeHint(const ConfigurableProvider& provider) const;
	// 表单下方的反馈行（校验失败、正在写入、结果提示）
	void setFeedback(const QString& text);
	void clearFeedback();

	// 按当前路由问一次 llm/discoverModels，候选落进模型 ID 输入框下方的下拉，点一条即回填
	void fetchModels();
	// 把刚取回的候选铺成下拉并等用户选；选了就写进输入框
	void showFetchedMenu(const QVector<DiscoveredModel>& models);
	// 按钮文案与可用性：没选中路由、没有服务端、或正在取的时候都不可点
	void updateFetchButton();

	// 列表级的瞬时提示（右键菜单那类不在表单里的操作），几秒后自动消失
	void setNotice(const QString& text);

	// 在成员行上弹出右键菜单；目前只有「删除」
	void showRowMenu(const QString& provider, const QString& modelId, const QPoint& globalPos);
	// 从服务端 settings 里删掉一个模型条目
	void removeModel(const QString& provider, const QString& modelId);

	// 按当前路由刷新 API Key 一行的引用名与“已配置/未配置”状态
	void refreshCredentialRow();
	// 只重绘引用行的文案（状态查询回来后调用）
	void refreshCredentialRowText();
	// 把整份模型写入服务端（凭据已在此前处理完）
	void writeModel(const ConfigurableProvider& provider, const SettingsNamespace& ns,
		const AddModelRequest& request);

	// 解析表单里的可选整数；空串返回 true 且不写值，非法返回 false
	static bool parseOptionalInt(const QLineEdit* edit, bool* hasValue, int* value);

	DshApiClient* m_api = nullptr;

	ServerModelView m_view;
	QVector<ModelInfo> m_rows;
	QSet<QString> m_expanded;   // 展开过的成员 key，刷新后恢复

	QLabel* m_status = nullptr;
	QLabel* m_notice = nullptr;
	QScrollArea* m_scroll = nullptr;
	QWidget* m_listContent = nullptr;
	QVBoxLayout* m_listLayout = nullptr;   // 只装模型行（刷新时整体清空）
	QWidget* m_rowsHost = nullptr;
	// 成员行右键菜单（懒创建，见 ModelListPanel.cpp 里的 ContextMenu）
	class ContextMenu;
	ContextMenu* m_rowMenu = nullptr;

	QPushButton* m_addButton = nullptr;
	QWidget* m_form = nullptr;
	QComboBox* m_providerCombo = nullptr;
	QLineEdit* m_idEdit = nullptr;
	// 「获取模型」按钮（在模型 ID 输入框右侧，见 ModelListPanel.cpp 的 buildForm）
	QPushButton* m_fetchButton = nullptr;
	// 取回候选后的下拉（懒创建，见 ModelListPanel.cpp 里的 FetchedModelsMenu）
	class FetchedModelsMenu;
	FetchedModelsMenu* m_fetchedMenu = nullptr;
	bool m_fetching = false;
	QLineEdit* m_nameEdit = nullptr;
	QLineEdit* m_contextEdit = nullptr;
	QLineEdit* m_maxTokensEdit = nullptr;
	QLabel* m_effortsLabel = nullptr;
	QLineEdit* m_effortsEdit = nullptr;
	QLabel* m_modalitiesLabel = nullptr;
	QCheckBox* m_imageInputBox = nullptr;
	QLabel* m_apiKeyLabel = nullptr;
	QLineEdit* m_apiKeyEdit = nullptr;
	QLabel* m_apiKeyRefLabel = nullptr;
	QLabel* m_routeHint = nullptr;
	QLabel* m_feedback = nullptr;
	QPushButton* m_submitButton = nullptr;

	// 当前表单路由的凭据引用与状态
	QString m_keyRef;
	QString m_apiKeyRefBase;   // 引用行的基础文案（状态另接在后面）
	CredentialStatus m_credential;
	bool m_submitting = false;
};
