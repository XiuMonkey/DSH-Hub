#include "ui/ModelListPanel.h"

#include "ui/LayoutUtils.h"
#include "core/ConnectionManager.h"
#include "network/DshApiClient.h"
#include "common/appearance/ThemeManager.h"
#include "ui/Tooltip.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDebug>
#include <QDialog>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>

namespace
{
	constexpr int kDetailNameWidth = 96;

	constexpr int kEntryHeaderPadding = 3;
	constexpr int kEntryHeaderMinHeight = 48;

	// 清单区最小高：只保证极端情况下不被压没
	constexpr int kMinListHeight = 120;

	constexpr int kMenuGap = 8;
	constexpr int kMenuScreenMargin = 12;
	constexpr int kMenuMinListHeight = 120;
	constexpr int kMenuMaxListHeight = 280;
	constexpr int kMenuBodyChrome = 10;
	constexpr int kMenuOptionMinHeight = 26;
	constexpr int kMenuFallbackWidth = 220;

	// pi-ai 适配器接受的思考档位名
	const char* const kThinkingLevels[] = {
		"off", "minimal", "low", "medium", "high", "xhigh", "max",
	};

	bool isKnownThinkingLevel(const QString& level)
	{
		for (const char* const known : kThinkingLevels) {
			if (level == QLatin1String(known))
				return true;
		}
		return false;
	}

	QString thinkingLevelList()
	{
		QStringList levels;
		for (const char* const known : kThinkingLevels)
			levels.append(QString::fromLatin1(known));
		return levels.join(QLatin1Char('/'));
	}

	QString formatCount(int value)
	{
		return QLocale::system().toString(value);
	}

	// 长 id 在下拉里被截断，展示名与容量只在这儿露面
	QString discoveredTooltip(const DiscoveredModel& model)
	{
		QStringList lines;
		lines.append(model.id);

		if (!model.name.isEmpty() && model.name != model.id)
			lines.append(model.name);

		if (model.hasContextWindow) {
			lines.append(QStringLiteral("%1: %2")
				.arg(qtTrId("model_context_window"), formatCount(model.contextWindow)));
		}
		if (model.hasMaxTokens) {
			lines.append(QStringLiteral("%1: %2")
				.arg(qtTrId("model_max_output"), formatCount(model.maxTokens)));
		}
		return lines.join(QLatin1Char('\n'));
	}
}

ModelListEntry::ModelListEntry(const ModelInfo& info, QWidget* parent)
	: QWidget(parent)
	, m_key(info.provider + QLatin1Char('\x1f') + info.id)
	, m_provider(info.provider)
	, m_modelId(info.id)
{
	setObjectName(QStringLiteral("modelListEntry"));
	setAttribute(Qt::WA_StyledBackground, true);

	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	m_header = new QPushButton(this);
	m_header->setObjectName(QStringLiteral("modelListEntryHeader"));
	m_header->setFlat(true);
	m_header->setCursor(Qt::PointingHandCursor);
	m_header->setFocusPolicy(Qt::NoFocus);
	// QPushButton 不给下限会被压到十几像素
	m_header->setMinimumHeight(kEntryHeaderMinHeight);

	auto* headerLayout = new QHBoxLayout(m_header);
	headerLayout->setContentsMargins(10, kEntryHeaderPadding, 10, kEntryHeaderPadding);
	headerLayout->setSpacing(8);

	auto* titleColumn = new QVBoxLayout;
	titleColumn->setContentsMargins(0, 0, 0, 0);
	titleColumn->setSpacing(0);

	auto* nameLabel = new QLabel(info.name, m_header);
	nameLabel->setObjectName(QStringLiteral("modelListEntryName"));
	nameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
	titleColumn->addWidget(nameLabel);

	auto* metaLabel = new QLabel(QStringLiteral("%1 · %2").arg(info.providerName, info.id), m_header);
	metaLabel->setObjectName(QStringLiteral("modelListEntryMeta"));
	metaLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
	titleColumn->addWidget(metaLabel);

	headerLayout->addLayout(titleColumn, 1);

	QString badge;
	if (info.userDeclared)
		badge = qtTrId("model_source_custom");
	else if (!info.declared)
		badge = qtTrId("model_source_adapter_builtin");
	if (!badge.isEmpty()) {
		auto* badgeLabel = new QLabel(badge, m_header);
		badgeLabel->setObjectName(QStringLiteral("modelListEntryBadge"));
		headerLayout->addWidget(badgeLabel, 0, Qt::AlignVCenter);
	}

	m_chevron = new QLabel(QStringLiteral("▸"), m_header);
	m_chevron->setObjectName(QStringLiteral("modelListEntryChevron"));
	headerLayout->addWidget(m_chevron, 0, Qt::AlignVCenter);

	layout->addWidget(m_header);

	m_detail = new QWidget(this);
	m_detail->setObjectName(QStringLiteral("modelListEntryDetail"));
	m_detail->setAttribute(Qt::WA_StyledBackground, true);

	auto* detailLayout = new QVBoxLayout(m_detail);
	detailLayout->setContentsMargins(10, 0, 10, 10);
	detailLayout->setSpacing(2);

	addDetailRow(detailLayout, m_detail, qtTrId("model_provider_route"), info.provider);
	addDetailRow(detailLayout, m_detail, qtTrId("model_id_label"), info.id);
	addDetailRow(detailLayout, m_detail, qtTrId("model_display_name"), info.name);

	if (!info.description.isEmpty())
		addDetailRow(detailLayout, m_detail, qtTrId("model_description_label"), info.description);

	addDetailRow(detailLayout, m_detail, qtTrId("model_context_window"),
		info.hasContextWindow ? formatCount(info.contextWindow) : QString());
	addDetailRow(detailLayout, m_detail, qtTrId("model_max_output"),
		info.hasMaxTokens ? formatCount(info.maxTokens) : QString());

	if (info.reasoningDisabled) {
		addDetailRow(detailLayout, m_detail, qtTrId("model_think_level"),
			qtTrId("model_think_not_offered"));
	}
	else if (info.hasReasoning) {
		QStringList names;
		for (const ReasoningLevel& level : info.levels)
			names.append(level.name);
		QString value = names.join(QStringLiteral(" / "));
		if (!info.defaultLevelId.isEmpty())
			value += qtTrId("model_default_value_fmt").arg(info.defaultLevelId);
		addDetailRow(detailLayout, m_detail, qtTrId("model_think_level"), value);
	}
	else {
		QStringList fallback;
		for (const ReasoningLevel& level : fallbackReasoningLevels())
			fallback.append(level.name);

		addDetailRow(detailLayout, m_detail, qtTrId("model_think_level"),
			qtTrId("model_think_undeclared_fmt").arg(fallback.join(QStringLiteral(" / "))));
	}

	addDetailRow(detailLayout, m_detail, qtTrId("model_config_source"),
		info.declared
		? (info.userDeclared
			? qtTrId("model_source_settings_user")
			: qtTrId("model_source_settings_bundled"))
		: qtTrId("model_source_adapter_only"));

	if (!info.settingsNs.isEmpty()) {
		QStringList path = info.settingsPath;
		path.append(QStringLiteral("models"));

		QString where = QStringLiteral("%1 · %2").arg(info.settingsNs, path.join(QLatin1Char('.')));
		if (!info.settingsWritable)
			where += qtTrId("model_settings_readonly_suffix");
		addDetailRow(detailLayout, m_detail, qtTrId("model_writeback_target"), where);
	}

	layout->addWidget(m_detail);
	m_detail->hide();

	dshRegister(QStringLiteral("ModelListPanel.header.%1").arg(m_key),
		m_header, qOverload<bool>(&QPushButton::clicked), this, [this]() {
			setExpanded(!isExpanded());
		});
}

QString ModelListEntry::key() const
{
	return m_key;
}

bool ModelListEntry::isExpanded() const
{
	return m_detail && m_detail->isVisible();
}

void ModelListEntry::setExpanded(bool expanded)
{
	if (!m_detail)
		return;

	m_detail->setVisible(expanded);
	if (m_chevron)
		m_chevron->setText(expanded ? QStringLiteral("▾") : QStringLiteral("▸"));

	emit expandedChanged(m_key, expanded);
}

void ModelListEntry::contextMenuEvent(QContextMenuEvent* event)
{
	emit contextMenuRequested(m_provider, m_modelId, event->globalPos());
	event->accept();
}

void ModelListEntry::addDetailRow(QVBoxLayout* layout, QWidget* parent, const QString& name, const QString& value)
{
	if (value.isEmpty())
		return;

	auto* row = new QWidget(parent);
	auto* rowLayout = new QHBoxLayout(row);
	rowLayout->setContentsMargins(0, 0, 0, 0);
	rowLayout->setSpacing(8);

	auto* nameLabel = new QLabel(name, row);
	nameLabel->setObjectName(QStringLiteral("modelListDetailName"));
	nameLabel->setFixedWidth(kDetailNameWidth);
	nameLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);

	auto* valueLabel = new QLabel(value, row);
	valueLabel->setObjectName(QStringLiteral("modelListDetailValue"));
	valueLabel->setWordWrap(true);
	valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

	rowLayout->addWidget(nameLabel);
	rowLayout->addWidget(valueLabel, 1);

	layout->addWidget(row);
}

// 成员行右键菜单。⚠️ 不用 QMenu：Windows 上圆角外会留直角背景

class ModelListPanel::ContextMenu : public QDialog
{
public:
	explicit ContextMenu(QWidget* parent)
		: QDialog(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
	{
		setAttribute(Qt::WA_TranslucentBackground);
		// 不抢激活，否则会被系统立即关闭
		setAttribute(Qt::WA_ShowWithoutActivating, true);

		auto* outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		outer->setSpacing(0);

		m_body = new QWidget(this);
		m_body->setObjectName(QStringLiteral("modelListContextMenu"));
		m_body->setAttribute(Qt::WA_StyledBackground, true);
		outer->addWidget(m_body);

		m_rows = new QVBoxLayout(m_body);
		m_rows->setContentsMargins(4, 4, 4, 4);
		m_rows->setSpacing(2);
	}

	void build(bool canRemove, const QString& blockedReason)
	{
		clear();
		m_chosen = false;

		auto* remove = new QPushButton(qtTrId("common_delete"), m_body);
		remove->setObjectName(QStringLiteral("modelListContextAction"));
		remove->setFlat(true);
		remove->setCursor(Qt::PointingHandCursor);
		// ⚠️ 行不参与焦点链，否则 Windows 会关掉弹层
		remove->setFocusPolicy(Qt::NoFocus);
		remove->setMinimumHeight(30);
		remove->setEnabled(canRemove);
		connect(remove, &QPushButton::clicked, this, [this]() {
			m_chosen = true;
			accept();
			});
		m_rows->addWidget(remove);

		if (!canRemove && !blockedReason.isEmpty()) {
			auto* hint = new QLabel(blockedReason, m_body);
			hint->setObjectName(QStringLiteral("modelListContextHint"));
			hint->setWordWrap(true);
			hint->setMaximumWidth(220);
			m_rows->addWidget(hint);
		}
	}

	bool removeChosen() const { return m_chosen; }

private:
	void clear()
	{
		// 要立刻摘离，否则旧行会被一并显示
		LayoutUtils::clearLayout(m_rows);
	}

	QWidget* m_body = nullptr;
	QVBoxLayout* m_rows = nullptr;
	bool m_chosen = false;
};

// 取回候选的下拉，点一条回填模型 ID

class ModelListPanel::FetchedModelsMenu : public QDialog
{
public:
	explicit FetchedModelsMenu(QWidget* parent)
		: QDialog(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
	{
		setAttribute(Qt::WA_TranslucentBackground);
		// 不抢激活，否则会被系统立即关闭
		setAttribute(Qt::WA_ShowWithoutActivating, true);

		auto* outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		outer->setSpacing(0);

		m_body = new QWidget(this);
		m_body->setObjectName(QStringLiteral("modelListFetchMenu"));
		m_body->setAttribute(Qt::WA_StyledBackground, true);
		outer->addWidget(m_body);

		auto* bodyLayout = new QVBoxLayout(m_body);
		bodyLayout->setContentsMargins(4, 4, 4, 4);
		bodyLayout->setSpacing(0);

		m_scroll = LayoutUtils::makeThemedScrollArea(m_body, QStringLiteral("modelListFetchScroll"));
		m_scroll->setFocusPolicy(Qt::NoFocus);

		m_content = new QWidget(m_scroll);
		m_content->setObjectName(QStringLiteral("modelListFetchMenuContent"));

		m_rows = new QVBoxLayout(m_content);
		m_rows->setContentsMargins(0, 0, 0, 0);
		m_rows->setSpacing(0);
		m_scroll->setWidget(m_content);

		bodyLayout->addWidget(m_scroll);
	}

	void build(const QVector<DiscoveredModel>& models, int width, int maxHeight)
	{
		clear();
		m_chosen.clear();

		const int popupWidth = width > 0 ? width : kMenuFallbackWidth;
		m_body->setFixedWidth(popupWidth);

		for (const DiscoveredModel& model : models) {
			auto* row = new QPushButton(model.id, m_content);
			row->setObjectName(QStringLiteral("modelListFetchOption"));
			row->setFlat(true);
			row->setCursor(Qt::PointingHandCursor);
			// ⚠️ 行不参与焦点链，否则 Windows 会关掉弹层
			row->setFocusPolicy(Qt::NoFocus);
			row->setMinimumHeight(kMenuOptionMinHeight);
			row->setToolTip(discoveredTooltip(model));

			connect(row, &QPushButton::clicked, this, [this, id = model.id]() {
				m_chosen = id;
				accept();
				});
			m_rows->addWidget(row);
		}

		// ⚠️ 不能拿布局 sizeHint：show() 前量出的高度不作数
		const int ceiling = qMax(kMenuMinListHeight, maxHeight);
		m_listHeight = qBound(kMenuMinListHeight, contentHeight(), ceiling);
		m_scroll->setFixedHeight(m_listHeight);

		// 直接给定尺寸，adjustSize() 可能还是旧值
		setFixedSize(popupWidth, m_listHeight + kMenuBodyChrome);
	}

	QString chosenId() const { return m_chosen; }

private:
	int contentHeight() const
	{
		int total = 0;
		int counted = 0;
		for (int i = 0; i < m_rows->count(); ++i) {
			QLayoutItem* item = m_rows->itemAt(i);
			QWidget* widget = item ? item->widget() : nullptr;
			if (!widget)
				continue;

			if (counted > 0)
				total += m_rows->spacing();
			total += widget->sizeHint().height();
			++counted;
		}
		return total;
	}

	void clear()
	{
		// 要立刻摘离，否则旧行会被一并显示
		LayoutUtils::clearLayout(m_rows);

		m_scroll->setMinimumHeight(0);
		m_scroll->setMaximumHeight(QWIDGETSIZE_MAX);
	}

	QWidget* m_body = nullptr;
	QScrollArea* m_scroll = nullptr;
	QWidget* m_content = nullptr;
	QVBoxLayout* m_rows = nullptr;
	int m_listHeight = kMenuMinListHeight;
	QString m_chosen;
};

ModelListPanel::ModelListPanel(DshApiClient* api, QWidget* parent)
	: QWidget(parent)
	, m_api(api)
{
	setObjectName(QStringLiteral("modelListPanel"));

	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(3);

	auto* title = new QLabel(qtTrId("model_list_title"), this);
	title->setObjectName(QStringLiteral("modelListTitle"));
	title->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

	auto* hint = new QLabel(qtTrId("model_list_desc"), this);
	hint->setWordWrap(true);
	hint->setObjectName(QStringLiteral("modelListHint"));

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	m_status->setObjectName(QStringLiteral("modelListStatus"));
	m_status->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

	m_notice = new QLabel(this);
	m_notice->setWordWrap(true);
	m_notice->setObjectName(QStringLiteral("modelListNotice"));
	m_notice->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	m_notice->hide();

	m_scroll = LayoutUtils::makeThemedScrollArea(this, QStringLiteral("modelListScroll"));
	// ⚠️ 不按内容设固定高度，否则会把窗口最小高度顶大
	m_scroll->setMinimumHeight(kMinListHeight);

	m_listContent = new QWidget(m_scroll);
	m_listContent->setObjectName(QStringLiteral("modelListContent"));

	// 表单放进滚动区，撑高的是滚动内容而非窗口
	auto* contentLayout = new QVBoxLayout(m_listContent);
	contentLayout->setContentsMargins(0, 0, 0, 0);
	contentLayout->setSpacing(4);

	m_rowsHost = new QWidget(m_listContent);
	m_rowsHost->setObjectName(QStringLiteral("modelListRows"));
	m_listLayout = new QVBoxLayout(m_rowsHost);
	m_listLayout->setContentsMargins(0, 0, 0, 0);
	m_listLayout->setSpacing(2);
	m_listLayout->addStretch(1);
	contentLayout->addWidget(m_rowsHost);

	buildForm(m_listContent);
	contentLayout->addWidget(m_form);

	// ⚠️ 末尾弹性空白必须留，否则表单多一行少一行就会整张上下挪
	contentLayout->addStretch(1);

	m_scroll->setWidget(m_listContent);

	m_addButton = new QPushButton(qtTrId("model_add"), this);
	m_addButton->setObjectName(QStringLiteral("modelListAddButton"));
	m_addButton->setCursor(Qt::PointingHandCursor);

	layout->addWidget(title);
	layout->addWidget(hint);
	layout->addWidget(m_status);
	layout->addWidget(m_notice);
	layout->addWidget(m_scroll, 1);
	layout->addWidget(m_addButton);

	dshRegister("ModelListPanel.001", m_addButton, qOverload<bool>(&QPushButton::clicked), this,
		[this]() {
			if (m_form && m_form->isVisible())
				closeForm();
			else
				openForm();
		});
}

void ModelListPanel::refresh()
{
	if (!m_api) {
		if (m_status)
			m_status->setText(qtTrId("model_server_not_ready"));
		return;
	}

	if (m_status)
		m_status->setText(qtTrId("model_loading_catalog"));

	// 面板常驻，请求可能晚于关闭设置
	QPointer<ModelListPanel> self(this);

	ModelSelectionService::fetchView(m_api,
		[self](const ServerModelView& view) {
			if (self)
				self->applyView(view);
		},
		[self](const DshApiClient::RpcError& error) {
			if (!self)
				return;
			qWarning().noquote() << QStringLiteral("[ModelList] session/modelCatalog failed:")
				<< error.code << error.message;
			if (self->m_status) {
				self->m_status->setText(
					qtTrId("model_catalog_load_failed_fmt").arg(error.code, error.message));
			}
		});
}

void ModelListPanel::applyView(const ServerModelView& view)
{
	m_view = view;
	m_rows = ModelSelectionService::buildModelInfos(view);

	populateRows();
	updateStatus();

	// 提交中不重建下拉，免得清掉用户选择
	if (!m_submitting)
		syncFormToRoute();
}

void ModelListPanel::clearRows()
{
	if (!m_listLayout)
		return;

	LayoutUtils::clearLayout(m_listLayout);
}

void ModelListPanel::populateRows()
{
	if (!m_listLayout)
		return;

	clearRows();

	QString lastProvider;
	for (const ModelInfo& info : m_rows) {
		if (info.provider != lastProvider) {
			lastProvider = info.provider;

			auto* header = new QLabel(info.providerName, m_listContent);
			header->setObjectName(QStringLiteral("modelListGroup"));
			m_listLayout->addWidget(header);
		}

		auto* entry = new ModelListEntry(info, m_listContent);
		if (m_expanded.contains(entry->key()))
			entry->setExpanded(true);

		dshRegister(QStringLiteral("ModelListPanel.expand.%1").arg(entry->key()), entry,
			&ModelListEntry::expandedChanged, this, [this](const QString& key, bool expanded) {
				if (expanded)
					m_expanded.insert(key);
				else
					m_expanded.remove(key);
			});
		dshRegister(QStringLiteral("ModelListPanel.menu.%1").arg(entry->key()), entry,
			&ModelListEntry::contextMenuRequested, this, &ModelListPanel::showRowMenu);

		m_listLayout->addWidget(entry);
	}

	if (m_rows.isEmpty()) {
		auto* empty = new QLabel(qtTrId("model_catalog_empty"), m_listContent);
		empty->setWordWrap(true);
		empty->setObjectName(QStringLiteral("modelListEmpty"));
		m_listLayout->addWidget(empty);
	}

	m_listLayout->addStretch(1);
}

void ModelListPanel::updateStatus()
{
	if (!m_status)
		return;

	QStringList notes;
	notes.append(qtTrId("model_catalog_summary_fmt").arg(m_rows.size()).arg(m_view.groups.size()));

	if (!m_view.settingsWritable)
		notes.append(qtTrId("model_add_readonly"));

	for (const ModelCatalogFailure& failure : m_view.failures) {
		notes.append(qtTrId("model_provider_load_failed_fmt")
			.arg(failure.name, failure.message));
	}

	m_status->setText(notes.join(QLatin1Char(' ')));

	if (m_addButton) {
		const bool canWrite = m_view.settingsWritable && !m_view.providers.isEmpty();
		m_addButton->setEnabled(canWrite);
		m_addButton->setToolTip(canWrite ? QString() : qtTrId("model_add_requires_api"));
	}
}

void ModelListPanel::buildForm(QWidget* parent)
{
	m_form = new QWidget(parent);
	m_form->setObjectName(QStringLiteral("modelListForm"));
	m_form->setAttribute(Qt::WA_StyledBackground, true);
	m_form->hide();

	auto* layout = new QVBoxLayout(m_form);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(6);

	auto* formTitle = new QLabel(qtTrId("model_add_title"), m_form);
	formTitle->setObjectName(QStringLiteral("modelListFormTitle"));
	layout->addWidget(formTitle);

	auto* grid = new QGridLayout;
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setHorizontalSpacing(8);
	grid->setVerticalSpacing(6);

	int row = 0;
	const auto addField = [&](const QString& name, QWidget* field) {
		auto* label = new QLabel(name, m_form);
		label->setObjectName(QStringLiteral("modelListFieldLabel"));
		grid->addWidget(label, row, 0, Qt::AlignRight | Qt::AlignVCenter);
		grid->addWidget(field, row, 1);
		++row;
		};

	m_providerCombo = new QComboBox(m_form);
	m_providerCombo->setObjectName(QStringLiteral("modelListField"));
	addField(qtTrId("model_provider_route"), m_providerCombo);

	m_idEdit = new QLineEdit(m_form);
	m_idEdit->setObjectName(QStringLiteral("modelListField"));
	m_idEdit->setPlaceholderText(qtTrId("model_id_hint"));

	auto* idRow = new QWidget(m_form);
	auto* idRowLayout = new QHBoxLayout(idRow);
	idRowLayout->setContentsMargins(0, 0, 0, 0);
	idRowLayout->setSpacing(6);
	idRowLayout->addWidget(m_idEdit, 1);

	m_fetchButton = new QPushButton(qtTrId("model_fetch_models"), idRow);
	m_fetchButton->setObjectName(QStringLiteral("modelListFetchButton"));
	m_fetchButton->setCursor(Qt::PointingHandCursor);
	idRowLayout->addWidget(m_fetchButton, 0);

	addField(qtTrId("model_id_label"), idRow);

	m_nameEdit = new QLineEdit(m_form);
	m_nameEdit->setObjectName(QStringLiteral("modelListField"));
	m_nameEdit->setPlaceholderText(qtTrId("model_display_name_hint"));
	addField(qtTrId("model_display_name"), m_nameEdit);

	m_contextEdit = new QLineEdit(m_form);
	m_contextEdit->setObjectName(QStringLiteral("modelListField"));
	m_contextEdit->setPlaceholderText(qtTrId("model_context_window_hint"));
	m_contextEdit->setValidator(new QIntValidator(1, 100000000, m_contextEdit));
	addField(qtTrId("model_context_window"), m_contextEdit);

	m_maxTokensEdit = new QLineEdit(m_form);
	m_maxTokensEdit->setObjectName(QStringLiteral("modelListField"));
	m_maxTokensEdit->setPlaceholderText(qtTrId("model_max_output_hint"));
	m_maxTokensEdit->setValidator(new QIntValidator(1, 100000000, m_maxTokensEdit));
	addField(qtTrId("model_max_output"), m_maxTokensEdit);

	m_effortsLabel = new QLabel(qtTrId("model_think_level"), m_form);
	m_effortsLabel->setObjectName(QStringLiteral("modelListFieldLabel"));
	grid->addWidget(m_effortsLabel, row, 0, Qt::AlignRight | Qt::AlignVCenter);

	m_effortsEdit = new QLineEdit(m_form);
	m_effortsEdit->setObjectName(QStringLiteral("modelListField"));
	m_effortsEdit->setPlaceholderText(qtTrId("model_think_level_hint"));
	grid->addWidget(m_effortsEdit, row, 1);
	++row;

	m_modalitiesLabel = new QLabel(qtTrId("model_input_modality"), m_form);
	m_modalitiesLabel->setObjectName(QStringLiteral("modelListFieldLabel"));
	grid->addWidget(m_modalitiesLabel, row, 0, Qt::AlignRight | Qt::AlignVCenter);

	m_imageInputBox = new QCheckBox(qtTrId("model_supports_image"), m_form);
	m_imageInputBox->setObjectName(QStringLiteral("modelListField"));
	grid->addWidget(m_imageInputBox, row, 1);
	++row;

	// 凭据写在服务端；填了才写，留空不动现有凭据
	m_apiKeyLabel = new QLabel(QStringLiteral("API Key"), m_form);
	m_apiKeyLabel->setObjectName(QStringLiteral("modelListFieldLabel"));
	grid->addWidget(m_apiKeyLabel, row, 0, Qt::AlignRight | Qt::AlignVCenter);

	m_apiKeyEdit = new QLineEdit(m_form);
	m_apiKeyEdit->setObjectName(QStringLiteral("modelListField"));
	m_apiKeyEdit->setEchoMode(QLineEdit::Password);
	m_apiKeyEdit->setPlaceholderText(qtTrId("model_credential_ref_hint"));
	grid->addWidget(m_apiKeyEdit, row, 1);
	++row;

	layout->addLayout(grid);

	m_apiKeyRefLabel = new QLabel(m_form);
	m_apiKeyRefLabel->setWordWrap(true);
	m_apiKeyRefLabel->setObjectName(QStringLiteral("modelListApiKeyRef"));
	layout->addWidget(m_apiKeyRefLabel);

	m_routeHint = new QLabel(m_form);
	m_routeHint->setWordWrap(true);
	m_routeHint->setObjectName(QStringLiteral("modelListRouteHint"));
	layout->addWidget(m_routeHint);

	// ⚠️ 反馈行高度固定，变高会让整张表单连字段一起挪
	m_feedback = new QLabel(m_form);
	m_feedback->setWordWrap(true);
	m_feedback->setObjectName(QStringLiteral("modelListFeedback"));
	// 先让样式生效再量高度，否则占位不够一行
	m_feedback->ensurePolished();
	m_feedback->setMinimumHeight(m_feedback->fontMetrics().height());
	layout->addWidget(m_feedback);

	auto* actions = new QHBoxLayout;
	actions->setContentsMargins(0, 0, 0, 0);
	actions->setSpacing(6);
	actions->addStretch(1);

	auto* cancelButton = new QPushButton(qtTrId("common_cancel"), m_form);
	cancelButton->setObjectName(QStringLiteral("modelListCancelButton"));
	cancelButton->setCursor(Qt::PointingHandCursor);

	m_submitButton = new QPushButton(qtTrId("common_add"), m_form);
	m_submitButton->setObjectName(QStringLiteral("modelListSubmitButton"));
	m_submitButton->setCursor(Qt::PointingHandCursor);

	actions->addWidget(cancelButton);
	actions->addWidget(m_submitButton);
	layout->addLayout(actions);

	dshRegister("ModelListPanel.002",
		cancelButton, qOverload<bool>(&QPushButton::clicked), this, [this]() { closeForm(); });
	dshRegister("ModelListPanel.003",
		m_submitButton, qOverload<bool>(&QPushButton::clicked), this, [this]() { submitForm(); });
	dshRegister("ModelListPanel.004",
		m_fetchButton, qOverload<bool>(&QPushButton::clicked), this, [this]() { fetchModels(); });

	dshRegister("ModelListPanel.005",
		m_providerCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
			if (m_submitting)
				return;
			syncFormToRoute();
		});
}

void ModelListPanel::setFeedback(const QString& text)
{
	if (!m_feedback)
		return;

	// 只换文字不显隐，免得表单重排版
	m_feedback->setText(text);
}

void ModelListPanel::clearFeedback()
{
	if (!m_feedback)
		return;

	m_feedback->clear();
	// 清掉悬停详情，免得看到旧错误
	m_feedback->setToolTip(QString());
}

void ModelListPanel::setNotice(const QString& text)
{
	m_notice->setText(text);
	m_notice->show();

	QTimer::singleShot(4000, this, [this]() {
		if (m_notice)
			m_notice->hide();
		});
}

void ModelListPanel::showRowMenu(const QString& provider, const QString& modelId, const QPoint& globalPos)
{
	if (provider.isEmpty() || modelId.isEmpty())
		return;

	if (!m_rowMenu)
		m_rowMenu = new ContextMenu(this);

	const QString blocked = m_view.settingsWritable
		? QString()
		: qtTrId("model_delete_readonly");
	m_rowMenu->build(m_view.settingsWritable, blocked);
	m_rowMenu->adjustSize();

	// 夹到屏幕内，避免贴边缘被切掉
	QPoint pos = globalPos;
	if (const QScreen* screen = QGuiApplication::screenAt(globalPos)) {
		const QRect available = screen->availableGeometry();
		pos.setX(qBound(available.left(), pos.x(),
			qMax(available.left(), available.right() - m_rowMenu->width())));
		pos.setY(qBound(available.top(), pos.y(),
			qMax(available.top(), available.bottom() - m_rowMenu->height())));
	}

	m_rowMenu->move(pos);
	m_rowMenu->exec();

	if (m_rowMenu->removeChosen())
		removeModel(provider, modelId);
}

void ModelListPanel::removeModel(const QString& provider, const QString& modelId)
{
	const ConfigurableProvider* entry = m_view.findProvider(provider);
	const SettingsNamespace* ns = entry ? m_view.findNamespace(entry->settingsNs) : nullptr;
	if (!entry || !ns) {
		qWarning().noquote() << QStringLiteral("[ModelList] cannot locate settings for")
			<< provider << modelId;
		setNotice(qtTrId("model_settings_path_missing_fmt").arg(provider));
		return;
	}

	// ⚠️ 复制一份供异步回调：原值指向 m_view 内部，刷新后失效
	const ConfigurableProvider providerCopy = *entry;
	const SettingsNamespace nsCopy = *ns;
	const QString settingsNs = providerCopy.settingsNs;

	QPointer<ModelListPanel> self(this);

	ModelSelectionService::removeModel(m_api, providerCopy, nsCopy, modelId,
		[self, settingsNs, provider, modelId](const SettingsNamespace& updated) {
			if (!self)
				return;

			bool replaced = false;
			for (SettingsNamespace& existing : self->m_view.namespaces) {
				if (existing.ns == updated.ns) {
					existing = updated;
					replaced = true;
					break;
				}
			}
			if (!replaced)
				self->m_view.namespaces.append(updated);

			self->m_rows = ModelSelectionService::buildModelInfos(self->m_view);
			self->populateRows();
			self->updateStatus();

			// 目录在适配器侧，删除要等 settings 热生效
			self->refresh();

			qInfo().noquote() << QStringLiteral("[ModelList] removed %1 -> %2/%3")
				.arg(settingsNs, provider, modelId);
			self->setNotice(qtTrId("model_deleted_fmt").arg(modelId));
		},
		[self, modelId](const DshApiClient::RpcError& error) {
			if (!self)
				return;

			qWarning().noquote() << QStringLiteral("[ModelList] settings/mutate (remove) failed:")
				<< error.code << error.message;
			self->setNotice(
				qtTrId("model_delete_failed_fmt").arg(modelId, error.code, error.message));
		});
}

void ModelListPanel::openForm()
{
	if (!m_form)
		return;

	syncFormToRoute();
	clearFeedback();

	m_form->show();
	if (m_idEdit)
		m_idEdit->setFocus();

	if (m_scroll && m_form) {
		QPointer<ModelListPanel> self(this);
		QTimer::singleShot(0, this, [self]() {
			if (self && self->m_scroll && self->m_form)
				self->m_scroll->ensureWidgetVisible(self->m_form, 0, 8);
			});
	}
}

void ModelListPanel::closeForm()
{
	if (!m_form)
		return;

	m_form->hide();
	clearFeedback();
}

const ConfigurableProvider* ModelListPanel::selectedProvider() const
{
	if (!m_providerCombo)
		return nullptr;

	const QString provider = m_providerCombo->currentData().toString();
	if (provider.isEmpty())
		return nullptr;

	return m_view.findProvider(provider);
}

QString ModelListPanel::routeHint(const ConfigurableProvider& provider) const
{
	QStringList parts;

	const SettingsNamespace* ns = m_view.findNamespace(provider.settingsNs);
	if (!ns) {
		parts.append(qtTrId("model_namespace_missing_fmt").arg(provider.settingsNs));
		return parts.join(QLatin1Char(' '));
	}

	const QJsonArray models = ModelSelectionService::configuredModels(*ns, provider.settingsPath);
	const bool userDeclared = ModelSelectionService::userDeclaresModels(*ns, provider.settingsPath);

	parts.append(qtTrId("model_write_progress_fmt").arg(provider.settingsNs,
		ModelSelectionService::modelsPath(provider).join(QLatin1Char('.')),
		QString::number(models.size())));

	if (provider.settingsNs.contains(QStringLiteral("pi-ai"))) {
		// pi-ai 的 models 是整体替换，会收窄公布范围
		if (!userDeclared)
			parts.append(qtTrId("model_write_warning_desc"));
	}
	else {
		parts.append(qtTrId("model_write_deepseek_warning"));
	}

	if (!m_view.settingsWritable)
		parts.append(qtTrId("model_write_readonly"));

	return parts.join(QLatin1Char(' '));
}

void ModelListPanel::syncFormToRoute()
{
	if (!m_providerCombo)
		return;

	const QString previous = m_providerCombo->currentData().toString();

	QSignalBlocker blocker(m_providerCombo);
	m_providerCombo->clear();

	for (const ConfigurableProvider& provider : m_view.providers) {
		QString label = provider.displayName;
		if (label != provider.provider)
			label += qtTrId("model_provider_route_suffix_fmt").arg(provider.provider);
		if (!provider.active)
			label += qtTrId("model_disabled_suffix");

		m_providerCombo->addItem(label, provider.provider);
	}

	if (!previous.isEmpty()) {
		const int index = m_providerCombo->findData(previous);
		if (index >= 0)
			m_providerCombo->setCurrentIndex(index);
	}

	const ConfigurableProvider* provider = selectedProvider();

	const bool piAi = provider && provider->settingsNs.contains(QStringLiteral("pi-ai"));
	const bool deepseek = provider && !provider->settingsNs.isEmpty() && !piAi;

	if (m_effortsLabel)
		m_effortsLabel->setVisible(piAi);
	if (m_effortsEdit)
		m_effortsEdit->setVisible(piAi);
	if (m_modalitiesLabel)
		m_modalitiesLabel->setVisible(deepseek);
	if (m_imageInputBox)
		m_imageInputBox->setVisible(deepseek);

	if (m_routeHint) {
		m_routeHint->setText(provider ? routeHint(*provider) : qtTrId("model_no_configurable_provider"));
	}

	refreshCredentialRow();

	if (m_submitButton)
		m_submitButton->setEnabled(provider != nullptr && m_view.settingsWritable);

	updateFetchButton();
}

void ModelListPanel::refreshCredentialRow()
{
	const ConfigurableProvider* provider = selectedProvider();

	m_keyRef.clear();
	m_apiKeyRefBase.clear();
	m_credential = CredentialStatus();
	if (m_apiKeyEdit) {
		m_apiKeyEdit->clear();
		m_apiKeyEdit->setPlaceholderText(qtTrId("model_credential_ref_hint"));
	}

	if (!provider) {
		if (m_apiKeyRefLabel)
			m_apiKeyRefLabel->clear();
		if (m_apiKeyLabel)
			m_apiKeyLabel->setVisible(false);
		if (m_apiKeyEdit)
			m_apiKeyEdit->setVisible(false);
		return;
	}

	if (m_apiKeyLabel)
		m_apiKeyLabel->setVisible(true);
	if (m_apiKeyEdit)
		m_apiKeyEdit->setVisible(true);

	// 引用名：profile 点名的优先，否则按约定派生
	const SettingsNamespace* ns = m_view.findNamespace(provider->settingsNs);
	const QString declared = ns
		? ModelSelectionService::profileApiKeyEnv(*ns, provider->settingsPath)
		: QString();
	m_keyRef = declared.isEmpty()
		? ModelSelectionService::deriveKeyRef(provider->provider)
		: declared;

	m_apiKeyRefBase = declared.isEmpty()
		? qtTrId("model_credential_derived_fmt").arg(m_keyRef)
		: qtTrId("model_credential_from_profile_fmt").arg(m_keyRef);
	refreshCredentialRowText();

	if (!m_api) {
		m_credential.ref = m_keyRef;
		m_credential.writable = true;
		if (m_apiKeyEdit)
			m_apiKeyEdit->setEnabled(true);
		refreshCredentialRowText();
		return;
	}

	QPointer<ModelListPanel> self(this);
	const QString ref = m_keyRef;
	if (m_apiKeyEdit)
		m_apiKeyEdit->setEnabled(false);

	ModelSelectionService::describeCredential(m_api, ref,
		[self, ref](const CredentialStatus& status) {
			if (!self || self->m_keyRef != ref)
				return;

			self->m_credential = status;
			if (self->m_apiKeyEdit)
				self->m_apiKeyEdit->setEnabled(status.writable);
			self->refreshCredentialRowText();
		},
		[self, ref](const DshApiClient::RpcError& error) {
			if (!self || self->m_keyRef != ref)
				return;

			qWarning().noquote() << QStringLiteral("[ModelList] credentials/describe failed:")
				<< error.code << error.message;
			// 问不到状态不阻塞填写
			self->m_credential = CredentialStatus();
			self->m_credential.ref = ref;
			self->m_credential.writable = true;
			if (self->m_apiKeyEdit)
				self->m_apiKeyEdit->setEnabled(true);
			self->refreshCredentialRowText();
		});
}

void ModelListPanel::refreshCredentialRowText()
{
	if (!m_apiKeyRefLabel)
		return;

	// ⚠️ 只有 known 才能下只读结论：writable 默认 false
	const bool readOnly = m_credential.known && !m_credential.writable;

	QString state;
	if (!m_credential.known) {
		state = qtTrId("model_credential_unknown_suffix");
	}
	else if (!m_credential.writable) {
		state = qtTrId("model_credential_readonly_suffix");
	}
	else if (m_credential.configured) {
		state = qtTrId("model_credential_configured_fmt")
			.arg(m_credential.source.isEmpty()
				? QString()
				: qtTrId("model_credential_source_fmt").arg(m_credential.source));
	}
	else {
		state = qtTrId("model_credential_unconfigured_suffix");
	}

	m_apiKeyRefLabel->setText(m_apiKeyRefBase + state);

	if (m_apiKeyEdit) {
		m_apiKeyEdit->setPlaceholderText(readOnly
			? qtTrId("model_credential_readonly_hint")
			: qtTrId("model_credential_ref_hint"));
	}
}

bool ModelListPanel::parseOptionalInt(const QLineEdit* edit, bool* hasValue, int* value)
{
	*hasValue = false;
	*value = 0;

	if (!edit)
		return true;

	const QString text = edit->text().trimmed();
	if (text.isEmpty())
		return true;

	bool ok = false;
	const int parsed = text.toInt(&ok);
	if (!ok || parsed <= 0)
		return false;

	*hasValue = true;
	*value = parsed;
	return true;
}

// 只读往返：候选铺进下拉，点一条回填 ID

void ModelListPanel::updateFetchButton()
{
	if (!m_fetchButton)
		return;

	const bool hasProvider = selectedProvider() != nullptr;

	m_fetchButton->setEnabled(m_api != nullptr && hasProvider && !m_fetching);
	m_fetchButton->setToolTip(m_fetching ? qtTrId("model_fetch_running") : qtTrId("model_fetch_hint"));
}

void ModelListPanel::fetchModels()
{
	// 临时诊断：采样几何
	{
		auto ticks = std::make_shared<int>(0);
		auto* sampler = new QTimer(this);
		sampler->setInterval(10);
		connect(sampler, &QTimer::timeout, this, [this, sampler, ticks]() {
			if (++(*ticks) >= 50) {
				sampler->stop();
				sampler->deleteLater();
			}
			});
		sampler->start();
	}

	if (m_fetching || !m_fetchButton || !m_fetchButton->isEnabled()) {
		return;
	}

	const ConfigurableProvider* provider = selectedProvider();
	if (!provider) {
		setFeedback(qtTrId("model_select_provider_first"));
		return;
	}

	const SettingsNamespace* ns = m_view.findNamespace(provider->settingsNs);
	if (!ns) {
		setFeedback(qtTrId("model_write_namespace_missing_fmt").arg(provider->settingsNs));
		return;
	}

	// 请求 = 路由 + profile 端点/协议 + 刚填的 key
	const QJsonObject request = ModelSelectionService::buildDiscoveryRequest(*provider, ns,
		m_apiKeyEdit ? m_apiKeyEdit->text().trimmed() : QString());

	// 回包可能晚于换路由，按路由 id 判断
	const QString providerId = provider->provider;
	const QString settingsNs = provider->settingsNs;

	m_fetching = true;
	updateFetchButton();
	setFeedback(qtTrId("model_fetch_running"));

	QPointer<ModelListPanel> self(this);

	ModelSelectionService::discoverModels(m_api, settingsNs, request,
		[self, providerId](const QVector<DiscoveredModel>& models) {
			if (!self)
				return;

			self->m_fetching = false;
			self->updateFetchButton();

			const ConfigurableProvider* current = self->selectedProvider();
			if (!current || current->provider != providerId)
				return;

			qInfo().noquote() << QStringLiteral("[ModelList] llm/discoverModels %1 -> %2 models")
				.arg(providerId).arg(models.size());

			if (models.isEmpty()) {
				self->setFeedback(qtTrId("model_fetch_empty"));
				return;
			}

			self->clearFeedback();
			self->showFetchedMenu(models);
		},
		[self, providerId](const DshApiClient::RpcError& error) {
			if (!self)
				return;

			self->m_fetching = false;
			self->updateFetchButton();

			qWarning().noquote() << QStringLiteral("[ModelList] llm/discoverModels failed:")
				<< providerId << error.code << error.message;

			// ⚠️ 只回本地化短句，英文原文挂到该行悬停提示里
			const QString raw = qtTrId("model_fetch_failed_fmt").arg(error.code, error.message);
			const bool unsupported =
				error.message.contains(QStringLiteral("no model discovery is registered"));

			self->setFeedback(unsupported ? qtTrId("model_fetch_unsupported") : raw);
			if (self->m_feedback)
				self->m_feedback->setToolTip(raw);
		});
}

void ModelListPanel::showFetchedMenu(const QVector<DiscoveredModel>& models)
{
	if (!m_idEdit || models.isEmpty())
		return;

	if (!m_fetchedMenu)
		m_fetchedMenu = new FetchedModelsMenu(this);

	// 下拉不超过输入框上下能腾出的高度
	QScreen* screen = QGuiApplication::screenAt(m_idEdit->mapToGlobal(m_idEdit->rect().center()));
	if (!screen)
		screen = QGuiApplication::primaryScreen();

	int maxListHeight = kMenuMaxListHeight;
	if (screen) {
		const QRect available = screen->availableGeometry();
		const int above = m_idEdit->mapToGlobal(QPoint(0, 0)).y() - available.top()
			- kMenuGap - kMenuScreenMargin;
		const int below = available.bottom() - m_idEdit->mapToGlobal(QPoint(0, m_idEdit->height())).y()
			- kMenuGap - kMenuScreenMargin;
		maxListHeight = qMin(kMenuMaxListHeight, qMax(above, below) - kMenuBodyChrome);
	}

	m_fetchedMenu->build(models, m_idEdit->width(), maxListHeight);

	// ⚠️ 纵横都夹进屏幕，否则系统会把弹层挪回来、看起来跳一下
	const QPoint inputTopLeft = m_idEdit->mapToGlobal(QPoint(0, 0));
	const int menuHeight = m_fetchedMenu->height();

	QPoint pos(inputTopLeft.x(), inputTopLeft.y() + m_idEdit->height() + kMenuGap);
	if (screen) {
		const QRect available = screen->availableGeometry();

		if (pos.y() + menuHeight > available.bottom() - kMenuScreenMargin)
			pos.setY(inputTopLeft.y() - menuHeight - kMenuGap);

		const int minX = available.left() + kMenuScreenMargin;
		const int maxX = qMax(minX, available.right() - m_fetchedMenu->width() - kMenuScreenMargin);
		pos.setX(qBound(minX, pos.x(), maxX));

		const int minY = available.top() + kMenuScreenMargin;
		const int maxY = qMax(minY, available.bottom() - menuHeight - kMenuScreenMargin);
		pos.setY(qBound(minY, pos.y(), maxY));
	}

	m_fetchedMenu->move(pos);
	m_fetchedMenu->exec();

	const QString chosen = m_fetchedMenu->chosenId();
	if (chosen.isEmpty())
		return;

	m_idEdit->setText(chosen);
	m_idEdit->setFocus();
	clearFeedback();
}

void ModelListPanel::submitForm()
{
	if (m_submitting || !m_api)
		return;

	const ConfigurableProvider* provider = selectedProvider();
	if (!provider) {
		setFeedback(qtTrId("model_select_provider_first"));
		return;
	}

	const SettingsNamespace* ns = m_view.findNamespace(provider->settingsNs);
	if (!ns) {
		setFeedback(qtTrId("model_write_namespace_missing_fmt").arg(provider->settingsNs));
		return;
	}

	AddModelRequest request;
	request.provider = provider->provider;
	request.id = m_idEdit ? m_idEdit->text().trimmed() : QString();
	request.name = m_nameEdit ? m_nameEdit->text().trimmed() : QString();

	if (request.id.isEmpty()) {
		setFeedback(qtTrId("model_id_required"));
		return;
	}

	if (!parseOptionalInt(m_contextEdit, &request.hasContextWindow, &request.contextWindow)
		|| !parseOptionalInt(m_maxTokensEdit, &request.hasMaxTokens, &request.maxTokens)) {
		setFeedback(qtTrId("model_numeric_invalid"));
		return;
	}

	// pi-ai：逗号分隔的档位名；空串 = 不声明
	if (provider->settingsNs.contains(QStringLiteral("pi-ai")) && m_effortsEdit) {
		const QString raw = m_effortsEdit->text().trimmed();
		if (!raw.isEmpty()) {
			const QStringList parts = raw.split(QLatin1Char(','), Qt::SkipEmptyParts);
			QStringList unknown;
			for (const QString& part : parts) {
				const QString level = part.trimmed();
				if (level.isEmpty())
					continue;
				if (!isKnownThinkingLevel(level)) {
					unknown.append(level);
					continue;
				}
				if (!request.reasoningEfforts.contains(level))
					request.reasoningEfforts.append(level);
			}

			if (!unknown.isEmpty()) {
				setFeedback(qtTrId("model_think_level_unknown_fmt")
					.arg(unknown.join(QStringLiteral(", ")), thinkingLevelList()));
				return;
			}
		}
	}

	if (m_imageInputBox)
		request.imageInput = m_imageInputBox->isChecked();

	// 凭据随本次新增一起处理，profile 点名的引用优先
	request.apiKeyRef = m_keyRef;
	request.recordApiKeyEnv = !m_keyRef.isEmpty()
		&& ModelSelectionService::profileApiKeyEnv(*ns, provider->settingsPath).isEmpty();

	const QString apiKey = m_apiKeyEdit ? m_apiKeyEdit->text().trimmed() : QString();

	m_submitting = true;
	if (m_submitButton)
		m_submitButton->setEnabled(false);
	setFeedback(apiKey.isEmpty() ? qtTrId("model_writing") : qtTrId("model_writing_credential"));

	// ⚠️ 复制一份供异步回调：原值指向 m_view 内部，刷新会失效
	const ConfigurableProvider providerCopy = *provider;
	const SettingsNamespace nsCopy = *ns;

	if (apiKey.isEmpty()) {
		writeModel(providerCopy, nsCopy, request);
		return;
	}

	// ⚠️ 先凭据后模型：key 写失败就不能落模型
	QPointer<ModelListPanel> self(this);
	const QString ref = request.apiKeyRef;

	ModelSelectionService::saveCredential(m_api, ref, apiKey,
		[self, providerCopy, nsCopy, request]() {
			if (self)
				self->writeModel(providerCopy, nsCopy, request);
		},
		[self, ref](const DshApiClient::RpcError& error) {
			if (!self)
				return;

			self->m_submitting = false;
			if (self->m_submitButton)
				self->m_submitButton->setEnabled(true);

			qWarning().noquote() << QStringLiteral("[ModelList] credentials/set failed:")
				<< error.code << error.message;

			self->setFeedback(qtTrId("model_credential_write_failed_fmt")
				.arg(ref, error.code, error.message));
		});
}

void ModelListPanel::writeModel(const ConfigurableProvider& provider, const SettingsNamespace& ns,
	const AddModelRequest& request)
{
	const QString providerId = provider.provider;
	const QString modelId = request.id;
	const QString settingsNs = provider.settingsNs;

	// 同 id 已在列表里 = 原地覆盖而非新增
	bool replacing = false;
	for (const auto& item : ModelSelectionService::configuredModels(ns, provider.settingsPath)) {
		if (item.toObject().value(QStringLiteral("id")).toString() == request.id) {
			replacing = true;
			break;
		}
	}

	QPointer<ModelListPanel> self(this);

	ModelSelectionService::addModel(m_api, provider, ns, request,
		[self, providerId, modelId, settingsNs, replacing](const SettingsNamespace& updated) {
			if (!self)
				return;

			self->m_submitting = false;
			self->m_credential = CredentialStatus(); // key 变了，状态待重查

			bool replaced = false;
			for (SettingsNamespace& existing : self->m_view.namespaces) {
				if (existing.ns == updated.ns) {
					existing = updated;
					replaced = true;
					break;
				}
			}
			if (!replaced)
				self->m_view.namespaces.append(updated);

			self->m_rows = ModelSelectionService::buildModelInfos(self->m_view);
			self->populateRows();
			self->updateStatus();

			// 目录在适配器侧，新条目要等 settings 热生效
			self->refresh();

			qInfo().noquote() << QStringLiteral("[ModelList] %1 %2 -> %3/%4")
				.arg(replacing ? QStringLiteral("updated") : QStringLiteral("added"),
					settingsNs, providerId, modelId);

			self->setFeedback(replacing
				? qtTrId("model_updated_fmt").arg(modelId)
				: qtTrId("model_added_fmt").arg(modelId));

			if (self->m_idEdit)
				self->m_idEdit->clear();
			if (self->m_nameEdit)
				self->m_nameEdit->clear();
			if (self->m_contextEdit)
				self->m_contextEdit->clear();
			if (self->m_maxTokensEdit)
				self->m_maxTokensEdit->clear();
			if (self->m_effortsEdit)
				self->m_effortsEdit->clear();

			emit self->modelAdded(providerId, modelId);
		},
		[self](const DshApiClient::RpcError& error) {
			if (!self)
				return;

			self->m_submitting = false;
			if (self->m_submitButton)
				self->m_submitButton->setEnabled(true);

			qWarning().noquote() << QStringLiteral("[ModelList] settings/mutate failed:")
				<< error.code << error.message;

			self->setFeedback(
				qtTrId("model_add_failed_fmt").arg(error.code, error.message));
		});
}
