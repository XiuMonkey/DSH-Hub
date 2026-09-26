#include "ui/ModelSelector.h"

#include "ui/LayoutUtils.h"
#include "core/ConnectionManager.h"
#include <algorithm>
#include "network/DshApiClient.h"
#include "ui/ShadowPanel.h"
#include "common/appearance/ThemeManager.h"
#include <QDebug>
#include <QDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>

namespace
{
	// kMenuBodyChrome 算可用高度时要扣掉；kMenuRadius 要跟 QSS 圆角一致
	constexpr int kMenuGap = 8;
	constexpr int kMenuWidth = 300;
	constexpr int kMenuBodyChrome = 12;
	constexpr int kMenuScreenMargin = 12;
	constexpr int kMenuMinListHeight = 120;
	constexpr int kChipHeight = 28;
	constexpr int kMenuRadius = 12;

	inline CardShadow::Spec menuShadowSpec()
	{
		CardShadow::Spec spec = CardShadow::level3();
		spec.radius = kMenuRadius;
		return spec;
	}
}

ModelSelectorRow::ModelSelectorRow(Kind kind, const QString& provider, const QString& id,
	const QString& title, const QString& subtitle, bool selected, QWidget* parent)
	: QPushButton(parent)
	, m_kind(kind)
	, m_provider(provider)
	, m_id(id)
{
	setObjectName(QStringLiteral("modelSelectorOption"));
	setFlat(true);
	// autoExclusive：同父控件下只能选一个，也不会再点一次就取消勾选
	setCheckable(true);
	setAutoExclusive(true);
	setChecked(selected);
	setCursor(Qt::PointingHandCursor);
	setMinimumHeight(38);
	// 不参与焦点链：出现可聚焦子控件时 Windows 会因激活变化立刻关掉弹出层
	setFocusPolicy(Qt::NoFocus);
	setAccessibleName(subtitle.isEmpty()
		? title
		: qtTrId("common_accessible_pair_fmt").arg(title, subtitle));

	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(8, 6, 8, 6);
	layout->setSpacing(8);

	auto* copy = new QVBoxLayout;
	copy->setContentsMargins(0, 0, 0, 0);
	copy->setSpacing(0);

	auto* nameLabel = new QLabel(title, this);
	nameLabel->setObjectName(QStringLiteral("modelSelectorOptionName"));
	copy->addWidget(nameLabel);

	if (!subtitle.isEmpty()) {
		auto* descLabel = new QLabel(subtitle, this);
		descLabel->setObjectName(QStringLiteral("modelSelectorOptionDesc"));
		descLabel->setWordWrap(true);
		copy->addWidget(descLabel);
	}
	layout->addLayout(copy, 1);

	m_check = new QLabel(QStringLiteral("✓"), this);
	m_check->setObjectName(QStringLiteral("modelSelectorOptionCheck"));
	m_check->setFixedWidth(18);
	m_check->setAlignment(Qt::AlignCenter);
	layout->addWidget(m_check, 0, Qt::AlignVCenter);

	updateCheckVisibility();
	// 行有多实例，index 带自身地址
	const QString rowIndex = QStringLiteral("ModelSelector.row.%1").arg(reinterpret_cast<quintptr>(this));
	dshRegister(rowIndex + QStringLiteral(".check"), this, qOverload<bool>(&QPushButton::toggled), this,
		[this]() { updateCheckVisibility(); });
	dshRegister(rowIndex + QStringLiteral(".click"), this, qOverload<bool>(&QPushButton::clicked), this,
		[this]() {
			if (m_kind == LevelKind)
				emit levelChosen(m_id);
			else
				emit modelChosen(m_provider, m_id);
		});
}

void ModelSelectorRow::updateCheckVisibility()
{
	if (m_check)
		m_check->setVisible(isChecked());
}

QSize ModelSelectorRow::sizeHint() const
{
	const QLayout* own = layout();
	return own ? own->sizeHint() : QPushButton::sizeHint();
}

QSize ModelSelectorRow::minimumSizeHint() const
{
	return sizeHint();
}

// 无边框透明弹窗 + 内层圆角主体（同 PopupWindow）
class ModelSelector::MenuDialog : public QDialog
{
public:
	explicit MenuDialog(QWidget* parent)
		: QDialog(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
	{
		setAttribute(Qt::WA_TranslucentBackground);
		// 不抢激活，避免被系统在激活变化时立即关闭
		setAttribute(Qt::WA_ShowWithoutActivating, true);

		auto* outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		outer->setSpacing(0);

		auto* body = new QWidget(this);
		body->setObjectName(QStringLiteral("modelSelectorMenu"));
		body->setAttribute(Qt::WA_StyledBackground, true);
		body->setFixedWidth(kMenuWidth);

		// 套阴影外壳（lv3），留白让整窗比本体大一圈
		auto* bodyPanel = new ShadowPanel(QStringLiteral("shadowFloat"), menuShadowSpec(), this);
		bodyPanel->setRadius(kMenuRadius);
		bodyPanel->setCard(body);
		outer->addWidget(bodyPanel);

		auto* layout = new QVBoxLayout(body);
		layout->setContentsMargins(6, 6, 6, 6);
		layout->setSpacing(2);

		m_scroll = LayoutUtils::makeThemedScrollArea(body, QStringLiteral("modelSelectorScroll"));
		m_scroll->setFocusPolicy(Qt::NoFocus);

		m_content = new QWidget(m_scroll);
		m_content->setObjectName(QStringLiteral("modelSelectorMenuContent"));

		m_options = new QVBoxLayout(m_content);
		m_options->setContentsMargins(0, 0, 0, 0);
		m_options->setSpacing(2);
		m_scroll->setWidget(m_content);

		// autoExclusive 按「同一父控件」分组：不分容器两节会互相取消勾选
		m_modelSection = makeSection(qtTrId("model_label"));
		m_levelSection = makeSection(qtTrId("model_think_depth"));

		layout->addWidget(m_scroll);
	}

	QWidget* modelSection() const { return m_modelSection; }
	QWidget* levelSection() const { return m_levelSection; }
	QVBoxLayout* modelRowsLayout() const { return m_modelRows; }
	QVBoxLayout* levelRowsLayout() const { return m_levelRows; }

	void addGroupLabel(const QString& text)
	{
		auto* label = new QLabel(text, m_modelSection);
		label->setObjectName(QStringLiteral("modelSelectorMenuGroup"));
		label->setContentsMargins(8, 6, 8, 2);
		m_modelRows->addWidget(label);
	}

	void addLevelHint(const QString& text)
	{
		auto* hint = new QLabel(text, m_levelSection);
		hint->setObjectName(QStringLiteral("modelSelectorOptionHint"));
		hint->setContentsMargins(8, 4, 8, 4);
		hint->setWordWrap(true);
		m_levelRows->addWidget(hint);
	}

	// 不能用布局 sizeHint()：弹窗未 show() 时子控件会被当空项跳过，菜单被压成一条
	int contentHeight() const
	{
		int total = 0;
		int counted = 0;
		for (int i = 0; i < m_options->count(); ++i) {
			QLayoutItem* item = m_options->itemAt(i);
			QWidget* widget = item ? item->widget() : nullptr;
			if (!widget)
				continue;
			if (counted > 0)
				total += m_options->spacing();
			total += widget->sizeHint().height();
			++counted;
		}
		return total;
	}

	// maxHeight 由调用方按 chip 上下能腾出的空间算出
	void fitContent(int maxHeight)
	{
		const int content = contentHeight();
		const int ceiling = qMax(kMenuMinListHeight, maxHeight);
		m_listHeight = qBound(kMenuMinListHeight, content, ceiling);
		m_scroll->setFixedHeight(m_listHeight);
	}

	// 必须按算出的值直接给尺寸：滚动区刚被 setFixedHeight 改过，adjustSize() 可能还拿旧 sizeHint
	int menuHeight() const { return m_listHeight + kMenuBodyChrome; }
	int menuWidth() const { return kMenuWidth; }

	void clearOptions()
	{
		// 只 deleteLater() 的话旧行仍是子控件，弹窗再 show() 会一并显示
		for (QVBoxLayout* rows : { m_modelRows, m_levelRows })
			LayoutUtils::clearLayout(rows);

		// 放开高度约束，免得上一份内容的固定高度残留
		m_scroll->setMinimumHeight(0);
		m_scroll->setMaximumHeight(QWIDGETSIZE_MAX);
	}

private:
	QWidget* makeSection(const QString& title)
	{
		auto* titleLabel = new QLabel(title, m_content);
		titleLabel->setObjectName(QStringLiteral("modelSelectorMenuTitle"));
		titleLabel->setContentsMargins(8, 3, 8, 5);
		m_options->addWidget(titleLabel);

		auto* section = new QWidget(m_content);
		section->setObjectName(QStringLiteral("modelSelectorMenuSection"));

		auto* rows = new QVBoxLayout(section);
		rows->setContentsMargins(0, 0, 0, 0);
		rows->setSpacing(2);
		m_options->addWidget(section);

		if (!m_modelSection) {
			m_modelRows = rows;
			return section;
		}

		m_levelRows = rows;
		return section;
	}

	QScrollArea* m_scroll = nullptr;
	QWidget* m_content = nullptr;
	QVBoxLayout* m_options = nullptr;
	// 由 fitContent 设定
	int m_listHeight = kMenuMinListHeight;
	QWidget* m_modelSection = nullptr;
	QWidget* m_levelSection = nullptr;
	QVBoxLayout* m_modelRows = nullptr;
	QVBoxLayout* m_levelRows = nullptr;
};

ModelSelector::ModelSelector(QWidget* parent)
	: QPushButton(parent)
{
	setObjectName(QStringLiteral("modelSelectorChip"));
	setCursor(Qt::PointingHandCursor);
	setFixedHeight(kChipHeight);
	setToolTip(qtTrId("model_select_title"));
	setFlat(true);
	// 不参与焦点链：chip 拿到焦点就一直显示焦点态；键盘用户走 UIA/MSAA Invoke
	setFocusPolicy(Qt::NoFocus);

	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(8, 0, 6, 0);
	layout->setSpacing(4);

	m_label = new QLabel(this);
	m_label->setObjectName(QStringLiteral("modelSelectorChipLabel"));
	m_value = new QLabel(this);
	m_value->setObjectName(QStringLiteral("modelSelectorChipValue"));
	m_chevron = new QLabel(QStringLiteral("▾"), this);
	m_chevron->setObjectName(QStringLiteral("modelSelectorChipChevron"));
	layout->addWidget(m_label);
	layout->addWidget(m_value);
	layout->addWidget(m_chevron);

	m_menu = new MenuDialog(this);
	dshRegister("ModelSelector.003", this, qOverload<bool>(&QPushButton::clicked), this,
		&ModelSelector::openMenu);

	hide();
}

QSize ModelSelector::sizeHint() const
{
	const QLayout* own = layout();
	return own ? own->sizeHint() : QPushButton::sizeHint();
}

QSize ModelSelector::minimumSizeHint() const
{
	return sizeHint();
}

bool ModelSelector::hasModels() const
{
	return m_hasDirectory && !m_directory.groups.isEmpty();
}

QString ModelSelector::currentModelName() const
{
	return m_directory.currentModelName();
}

QString ModelSelector::currentLevelName() const
{
	return m_directory.currentLevelName();
}

QString ModelSelector::currentLevelId() const
{
	if (!m_directory.current.reasoningEffort.isEmpty())
		return m_directory.current.reasoningEffort;
	const ReasoningLevel* level = m_directory.currentLevel();
	return level ? level->id : QString();
}

void ModelSelector::setSession(DshApiClient* api, const QString& sessionId)
{
	if (api == m_api && sessionId == m_sessionId && m_hasDirectory)
		return;

	m_api = api;
	m_sessionId = sessionId;

	// 会话换了：上一个会话的选择作废
	m_hasSessionSelection = false;
	m_sessionSelection = ModelSelection();

	clearDirectory();

	if (!m_api || m_sessionId.isEmpty())
		return;

	refresh();
}

void ModelSelector::refresh()
{
	if (!m_api || m_sessionId.isEmpty())
		return;

	// 会话可能在请求返回前被销毁（切主题重建主窗口），用 QPointer 兜住
	QPointer<ModelSelector> self(this);
	const QString sessionId = m_sessionId;

	ModelSelectionService::fetch(m_api, sessionId,
		[self, sessionId](const SessionModelDirectory& directory) {
			if (!self || self->m_sessionId != sessionId)
				return; // 已经切到别的会话，丢弃过期结果
			self->applyDirectory(directory);
		},
		[self, sessionId](const DshApiClient::RpcError& error) {
			if (!self || self->m_sessionId != sessionId)
				return;
			qWarning().noquote() << QStringLiteral("[ModelSelector] session/modelCatalog failed:")
				<< error.code << error.message;
			self->clearDirectory();
		});
}

void ModelSelector::applyDirectory(const SessionModelDirectory& directory)
{
	m_directory = directory;

	// 目录只给部署默认值；会话自己的选择优先，只要模型仍在目录里就覆盖 current
	if (m_hasSessionSelection && !m_sessionSelection.provider.isEmpty()) {
		const bool known = std::any_of(m_directory.groups.cbegin(), m_directory.groups.cend(),
			[this](const ModelProviderGroup& group) {
				if (group.id != m_sessionSelection.provider)
					return false;
				return std::any_of(group.models.cbegin(), group.models.cend(),
					[this](const ModelOption& option) { return option.id == m_sessionSelection.model; });
			});
		if (known)
			m_directory.current = m_sessionSelection;
		else
			qInfo().noquote() << QStringLiteral("[ModelSelector] session selection not in catalog, keeping default:")
			<< m_sessionSelection.provider << m_sessionSelection.model;
	}

	m_hasDirectory = true;
	updateChip();
}

void ModelSelector::overrideCurrentSelection(const QString& provider, const QString& model,
	const QString& reasoningEffort)
{
	if (provider.isEmpty() || model.isEmpty()) {
		// 服务端没记录过：保持目录默认值
		m_hasSessionSelection = false;
		return;
	}

	m_sessionSelection.provider = provider;
	m_sessionSelection.model = model;
	m_sessionSelection.reasoningEffort = reasoningEffort;
	m_hasSessionSelection = true;

	if (m_hasDirectory) {
		m_directory.current = m_sessionSelection;
		updateChip();
	}

	qInfo().noquote() << QStringLiteral("[ModelSelector] session selection applied:")
		<< provider << model << reasoningEffort;
}

void ModelSelector::clearDirectory()
{
	m_directory = SessionModelDirectory();
	m_hasDirectory = false;
	updateChip();
}

void ModelSelector::updateChip()
{
	const bool available = hasModels();

	// chip 显示当前模型，档位跟在后面（没公布档位时整段省略）
	const QString model = m_directory.currentModelName();
	const QString level = m_directory.currentLevelName();

	m_label->setText(model);
	m_label->setVisible(!model.isEmpty());

	m_value->setText(level);
	m_value->setVisible(!level.isEmpty());

	setAccessibleName(level.isEmpty()
		? qtTrId("model_name_fmt").arg(model)
		: qtTrId("model_name_think_fmt").arg(model, level));

	setVisible(available);
	if (available)
		updateGeometry();
}

void ModelSelector::buildMenu(int maxListHeight)
{
	m_menu->clearOptions();

	const QString currentProvider = m_directory.current.provider;
	const QString currentModel = m_directory.current.model;

	for (const ModelProviderGroup& group : m_directory.groups) {
		if (m_directory.groups.size() > 1)
			m_menu->addGroupLabel(group.name);

		for (const ModelOption& option : group.models) {
			const bool selected = group.id == currentProvider && option.id == currentModel;

			QString subtitle = option.description;
			if (subtitle.isEmpty())
				subtitle = option.id;

			// 父控件用「模型」节的容器，单选组因此限定在这节内
			auto* row = new ModelSelectorRow(ModelSelectorRow::ModelKind, group.id, option.id,
				option.name, subtitle, selected, m_menu->modelSection());

			dshRegister(QStringLiteral("ModelSelector.model.%1.%2").arg(group.id, option.id),
				row, &ModelSelectorRow::modelChosen, m_menu,
				[this](const QString& provider, const QString& model) {
					m_menu->accept();
					chooseModel(provider, model);
				});
			m_menu->modelRowsLayout()->addWidget(row);
		}
	}

	if (m_directory.groups.isEmpty())
		m_menu->addGroupLabel(qtTrId("model_none_published"));

	// 没公布档位时用通用四档兜底，必须注明不是适配器公布的
	const QVector<ReasoningLevel> levels = m_directory.selectableLevels();
	if (m_directory.usesFallbackLevels())
		m_menu->addLevelHint(qtTrId("model_think_generic_note"));

	const QString selectedId = currentLevelId();

	for (const ReasoningLevel& level : levels) {
		auto* row = new ModelSelectorRow(ModelSelectorRow::LevelKind, QString(), level.id,
			level.name, level.description, level.id == selectedId, m_menu->levelSection());

		dshRegister(QStringLiteral("ModelSelector.level.%1").arg(level.id),
			row, &ModelSelectorRow::levelChosen, m_menu,
			[this](const QString& levelId) {
				m_menu->accept();
				chooseLevel(levelId);
			});
		m_menu->levelRowsLayout()->addWidget(row);
	}

	m_menu->fitContent(maxListHeight);
}

void ModelSelector::openMenu()
{
	if (!hasModels())
		return;
	// 防重入：菜单开着时不再重建（旧行会残留）
	if (m_menu->isVisible())
		return;

	const QScreen* screen = QGuiApplication::screenAt(mapToGlobal(rect().center()));
	if (!screen)
		screen = QGuiApplication::primaryScreen();

	int maxListHeight = kMenuMinListHeight;
	if (screen) {
		const QRect available = screen->availableGeometry();
		const int above = mapToGlobal(QPoint(0, 0)).y() - available.top()
			- kMenuGap - kMenuScreenMargin;
		const int below = available.bottom() - mapToGlobal(QPoint(0, height())).y()
			- kMenuGap - kMenuScreenMargin;
		maxListHeight = qMax(above, below) - kMenuBodyChrome;
	}
	// 阴影外壳也占地方，先扣掉
	const QMargins menuShadowPad = CardShadow::padding(menuShadowSpec());
	maxListHeight -= menuShadowPad.top() + menuShadowPad.bottom();

	buildMenu(maxListHeight);
	// 按算出的高度直接设定；= 本体尺寸 + 阴影留白
	m_menu->setFixedSize(m_menu->menuWidth() + menuShadowPad.left() + menuShadowPad.right(),
		m_menu->menuHeight() + menuShadowPad.top() + menuShadowPad.bottom());

	// 本体底边贴在 chip 上方 kMenuGap 处；整窗比本体大一圈，定位要把阴影留白补回来
	const QPoint above = mapToGlobal(QPoint(-menuShadowPad.left(),
		-m_menu->height() - kMenuGap + menuShadowPad.bottom()));
	QPoint pos = above;

	if (screen) {
		const QRect available = screen->availableGeometry();
		if (pos.x() + m_menu->width() > available.right())
			pos.setX(mapToGlobal(QPoint(width(), 0)).x() - m_menu->width() + menuShadowPad.right());
		pos.setX(qBound(available.left(), pos.x(), qMax(available.left(), available.right() - m_menu->width())));
		if (pos.y() < available.top())
			pos.setY(mapToGlobal(QPoint(0, height() + kMenuGap)).y() - menuShadowPad.top());
	}

	m_menu->move(pos);

	m_chevron->setText(QStringLiteral("▴"));
	m_menu->exec();
	m_chevron->setText(QStringLiteral("▾"));
}

// 换模型/换档位同一套动作：乐观更新 chip → session/selectModel → 以回显为准，失败回拉目录
void ModelSelector::submitSelection(const ModelSelection& selection, bool reloadDirectory,
	const std::function<void(const ModelSelection& selected)>& onAccepted)
{
	m_directory.current = selection;
	// 记下会话级选择
	m_sessionSelection = selection;
	m_hasSessionSelection = true;
	updateChip();

	QPointer<ModelSelector> self(this);
	const QString sessionId = m_sessionId;

	ModelSelectionService::select(m_api, sessionId, selection,
		[self, sessionId, reloadDirectory, onAccepted](const ModelSelection& selected) {
			if (!self || self->m_sessionId != sessionId)
				return; // 已经切到别的会话，丢弃过期结果

			self->m_directory.current = selected;
			self->updateChip();
			if (reloadDirectory)
				self->refresh();
			if (onAccepted)
				onAccepted(selected);
		},
		[self, sessionId](const DshApiClient::RpcError& error) {
			if (!self || self->m_sessionId != sessionId)
				return;
			qWarning().noquote() << QStringLiteral("[ModelSelector] session/selectModel failed:")
				<< error.code << error.message;
			self->refresh();
		});
}

void ModelSelector::chooseModel(const QString& provider, const QString& modelId)
{
	if (provider.isEmpty() || modelId.isEmpty() || !m_api || m_sessionId.isEmpty())
		return;
	// 点的是当前模型：避免白跑 RPC
	if (provider == m_directory.current.provider && modelId == m_directory.current.model)
		return;

	ModelSelection selection;
	selection.provider = provider;
	selection.model = modelId;
	// 档位留空 = 由新模型的适配器默认值决定，沿用旧档位会被服务端拒绝
	selection.reasoningEffort.clear();

	submitSelection(selection, /*reloadDirectory=*/true, [this](const ModelSelection& selected) {
		qInfo().noquote() << QStringLiteral("[ModelSelector] -> %1/%2")
			.arg(selected.provider, selected.model);
		emit modelChanged(selected.provider, selected.model);
		});
}

void ModelSelector::chooseLevel(const QString& levelId)
{
	if (levelId.isEmpty() || !m_api || m_sessionId.isEmpty())
		return;

	ModelSelection selection = m_directory.current;
	selection.reasoningEffort = levelId;

	submitSelection(selection, /*reloadDirectory=*/false, [this](const ModelSelection& selected) {
		qInfo().noquote() << QStringLiteral("[ModelSelector] %1/%2 -> %3")
			.arg(selected.provider, selected.model,
				selected.reasoningEffort.isEmpty() ? qtTrId("common_default_suffix") : selected.reasoningEffort);
		emit levelChanged(selected.reasoningEffort);
		});
}
