#pragma once

// ------------------------------------------------------------------
// ModelSelector.h
// ------------------------------------------------------------------
// 输入框底部控制行里的“模型 / 思考深度”控件（仿原生 DSH composer 的 chip）：
// 选择模型与选择思考档位是同一个控件、同一份目录数据（原来是只管档位的
// ThinkingDepthSelector，改名并扩写后模型选择也归它）。
//   - 收起状态是一枚 chip：`当前模型名` + `当前档位名` + 尖角；
//   - 点击后在 chip 正上方弹出菜单（上拉框），高度按内容适配，装不下才滚动：
//       上段「模型」——按提供方分组列出目录里的模型，当前模型带勾选；
//       下段「思考深度」——当前模型适配器公布的档位，当前档位带勾选；
//   - 换模型时把档位清空（交给新模型的默认档位），换档位时沿用当前 provider/model；
//     两次操作都走 session.selectModel，以服务端回显为准。
//   - 目录来自部署级的 session/modelCatalog；会话自己的选择由 DSHHub 从
//     session/list 的投影里取出来推入（overrideCurrentSelection）。
//     没有会话时整个控件隐藏。
//
// 网络逻辑在 common 的 ModelSelectionService，这里只负责画与交互。
// ------------------------------------------------------------------

#include "common/session/ModelSelectionService.h"

#include <QPushButton>
#include <QSize>
#include <QString>
#include <QWidget>
#include <functional>

class QLabel;
class QScrollArea;
class QVBoxLayout;
class DshApiClient;

// 弹窗里的一行（模型行与思考档位行共用外观；原生菜单同样是 <button>，
// 用按钮便于键盘/无障碍操作）
class ModelSelectorRow : public QPushButton
{
	Q_OBJECT

public:
	// 行的种类：决定这一行被点中时发出哪个信号
	enum Kind
	{
		ModelKind, // provider + id 是一个模型
		LevelKind, // provider 为空，id 是一个思考档位
	};

	ModelSelectorRow(
		Kind kind,
		const QString& provider,
		const QString& id,
		const QString& title,
		const QString& subtitle,
		bool selected,
		QWidget* parent = nullptr);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

signals:
	// 模型行被点中
	void modelChosen(const QString& provider, const QString& model);
	// 思考档位行被点中
	void levelChosen(const QString& levelId);

private:
	void updateCheckVisibility();

	Kind m_kind;
	QString m_provider;
	QString m_id;
	QLabel* m_check = nullptr;
};

// chip 本体：点击展开/收起上拉菜单。
// 用 QPushButton 而非裸 QWidget —— 与原生 composer 的 <button> 语义一致，
// 同时天然获得键盘焦点与无障碍/自动化可调用性（内容由子标签绘制）。
class ModelSelector : public QPushButton
{
	Q_OBJECT

public:
	explicit ModelSelector(QWidget* parent = nullptr);

	// 尺寸跟随内部标签布局（QPushButton 自身没有文本，默认 sizeHint 会偏小）
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

	// 绑定当前会话；sessionId 为空（或没有会话）时控件隐藏
	void setSession(DshApiClient* api, const QString& sessionId);

	// 用服务端最新数据刷新（会话切换、连接建立后调用）
	void refresh();

	/**
	 * 用"该会话记录的选择"覆盖 chip 的当前值。
	 *
	 * 0.1.5 的 modelCatalog 只给部署默认值（default），会话自己的选择在
	 * session/list 行的 projections.values.modelSelection 里；DSHHub 从
	 * SessionCatalog 取出来喂给这里。provider/model 为空表示"服务端还没记录"，
	 * 此时保持目录给的默认值不动。
	 */
	void overrideCurrentSelection(const QString& provider, const QString& model,
		const QString& reasoningEffort);

signals:
	// 用户换了模型（服务端已接受）
	void modelChanged(const QString& provider, const QString& model);
	// 用户改了思考档位（服务端已接受）
	void levelChanged(const QString& levelId);

private:
	bool hasModels() const;
	QString currentModelName() const;
	QString currentLevelName() const;
	QString currentLevelId() const;

	void applyDirectory(const SessionModelDirectory& directory);
	void clearDirectory();
	void openMenu();
	void chooseModel(const QString& provider, const QString& modelId);
	void chooseLevel(const QString& levelId);
	// 提交一次选择（换模型与换档位共用）：乐观更新 chip -> 发 RPC ->
	// 以服务端回显为准，失败回到服务端事实。
	// reloadDirectory：成功后是否重新拉目录（换模型后档位集合可能变）
	void submitSelection(const ModelSelection& selection, bool reloadDirectory,
		const std::function<void(const ModelSelection& selected)>& onAccepted);
	// 依据当前目录重建菜单内容；maxListHeight 是清单区能用的最大高度
	// （按 chip 上下能腾出的空间算出，装得下就整段显示，装不下才滚动）
	void buildMenu(int maxListHeight);
	void updateChip();

	class MenuDialog;

	DshApiClient* m_api = nullptr;
	QString m_sessionId;
	SessionModelDirectory m_directory;
	// 该会话记录的模型选择（来自 session/list 投影）；目录刷新后仍以它为准
	ModelSelection m_sessionSelection;
	bool m_hasSessionSelection = false;
	bool m_hasDirectory = false;

	QLabel* m_label = nullptr;   // 当前模型名
	QLabel* m_value = nullptr;   // 当前思考档位名（该模型没公布档位时为空）
	QLabel* m_chevron = nullptr;
	MenuDialog* m_menu = nullptr;
};
