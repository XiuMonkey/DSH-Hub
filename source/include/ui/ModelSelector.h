#pragma once

// 输入框底部控制行里的“模型 / 思考深度”chip：模型选择与思考档位共用同一个控件、同一份目录数据。
// 点击后在 chip 正上方弹出上拉菜单（高度按内容适配，装不下才滚动），上段按提供方分组列模型、下段列当前模型的档位。
// 换模型会把档位清空交给新模型默认档位，两者都走 session.selectModel 并以服务端回显为准；没有会话时整个控件隐藏。

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

// 弹窗里的一行：用按钮而非裸 QWidget，便于键盘/无障碍操作（原生菜单同样是 <button>）
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
	void modelChosen(const QString& provider, const QString& model);
	void levelChosen(const QString& levelId);

private:
	void updateCheckVisibility();

	Kind m_kind;
	QString m_provider;
	QString m_id;
	QLabel* m_check = nullptr;
};

// chip 本体：点击展开/收起上拉菜单；用 QPushButton 而非裸 QWidget，与原生 composer 的 <button> 语义一致，并天然获得键盘焦点与无障碍可调用性。
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

	// 用该会话记录过的选择覆盖 chip；provider/model 为空表示服务端还没记录，保持目录给的部署默认值
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
	// 提交一次选择（乐观更新 chip -> 发 RPC -> 以服务端回显为准，失败回到服务端事实）；reloadDirectory = 成功后是否重拉目录（换模型后档位集合可能变）
	void submitSelection(const ModelSelection& selection, bool reloadDirectory,
		const std::function<void(const ModelSelection& selected)>& onAccepted);
	// 依据当前目录重建菜单内容；maxListHeight 按 chip 上下能腾出的空间算出，装得下就整段显示，装不下才滚动
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
