#include "ModelListPanel.h"

#include "DshApiClient.h"
#include "ThemeManager.h"

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
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
	// 详情区“名称：值”两列
	constexpr int kDetailNameWidth = 96;

	// 成员表头：上下内边距与最小高度。
	// 高度下限要能装下「14px 名称 + 12px 说明 + 上下内边距」，见 ModelListEntry 构造里的说明。
	constexpr int kEntryHeaderPadding = 3;
	constexpr int kEntryHeaderMinHeight = 48;

	// 清单区的最小高度：只是让布局在极端情况下不至于把这块压没；
	// 正常情况下它占满面板剩余空间，装得下就不出滚动条，装不下才滚动。
	constexpr int kMinListHeight = 120;

	// pi-ai 适配器接受的思考档位名（THINKING_LEVELS）；表单据此校验用户输入，
	// 免得写出一个适配器解析不了的档位、整份分节被 settings 拒绝。
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

	// “off/minimal/low/medium/high/xhigh/max”，供校验失败时提示可用取值
	QString thinkingLevelList()
	{
		QStringList levels;
		for (const char* const known : kThinkingLevels)
			levels.append(QString::fromLatin1(known));
		return levels.join(QLatin1Char('/'));
	}

	QString formatCount(int value)
	{
		// 容量动辄上百万，加千位分隔可读性差别很大
		return QLocale::system().toString(value);
	}
}

// ------------------------------------------------------------------
// ModelListEntry
// ------------------------------------------------------------------

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

	// 表头就是一枚按钮：整行可点，语义与列表项一致
	m_header = new QPushButton(this);
	m_header->setObjectName(QStringLiteral("modelListEntryHeader"));
	m_header->setFlat(true);
	m_header->setCursor(Qt::PointingHandCursor);
	m_header->setFocusPolicy(Qt::NoFocus);
	// QPushButton 的高度由样式按“按钮文本/图标”算出来，不会因为里面塞了一个布局就变高
	// ——不显式给下限时，这一行会被压到十几像素，名称与说明各只剩一条线，
	// 看起来就是“文字上下被遮住”。下限取「两行字 + 上下内边距」，不留多余留白。
	m_header->setMinimumHeight(kEntryHeaderMinHeight);

	auto* headerLayout = new QHBoxLayout(m_header);
	headerLayout->setContentsMargins(10, kEntryHeaderPadding, 10, kEntryHeaderPadding);
	headerLayout->setSpacing(8);

	auto* titleColumn = new QVBoxLayout;
	titleColumn->setContentsMargins(0, 0, 0, 0);
	titleColumn->setSpacing(0);

	auto* nameLabel = new QLabel(info.name, m_header);
	nameLabel->setObjectName(QStringLiteral("modelListEntryName"));
	// 纵向 Minimum：sizeHint 是硬下限，布局不能把标签压得比一行字还矮
	nameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
	titleColumn->addWidget(nameLabel);

	auto* metaLabel = new QLabel(
		QStringLiteral("%1 · %2").arg(info.providerName, info.id), m_header);
	metaLabel->setObjectName(QStringLiteral("modelListEntryMeta"));
	metaLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
	titleColumn->addWidget(metaLabel);

	headerLayout->addLayout(titleColumn, 1);

	// 只在“不是随附默认”时打标：用户自己加/改的，或只在 settings 里声明的
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

	// ---------------- 详情 ----------------
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
		// 与输入框底的选择器保持一致：未公布时那里给的是通用四档，这里也写明
		QStringList fallback;
		for (const ReasoningLevel& level : fallbackReasoningLevels())
			fallback.append(level.name);

		addDetailRow(detailLayout, m_detail, qtTrId("model_think_level"),
			qtTrId("model_think_undeclared_fmt")
			.arg(fallback.join(QStringLiteral(" / "))));
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

	connect(m_header, &QPushButton::clicked, this, [this]() {
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
	// 表头与详情区都算这个成员：子控件不处理该事件时会冒泡到这里
	emit contextMenuRequested(m_provider, m_modelId, event->globalPos());
	event->accept();
}

void ModelListEntry::addDetailRow(
	QVBoxLayout* layout, QWidget* parent, const QString& name, const QString& value)
{
	// 值缺失就整行不建：详情里出现一堆“—”没有信息量
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

// ------------------------------------------------------------------
// ModelListPanel::ContextMenu
// ------------------------------------------------------------------
// 成员行的右键菜单。目前只有「删除」一项。
//
// 不用 QMenu：Windows 上原生 QMenu 弹窗即使开了 WA_TranslucentBackground，
// 圆角外仍会留一块直角背景（Sidebar 里踩过同样的坑）。这里与模型选择器的
// 上拉菜单同做法：无边框 Popup + 内层圆角主体 + 真正透明的外圈。
// ------------------------------------------------------------------

class ModelListPanel::ContextMenu : public QDialog
{
public:
	explicit ContextMenu(QWidget* parent)
		: QDialog(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
	{
		setAttribute(Qt::WA_TranslucentBackground);
		// 只做短暂浮层：不抢激活，避免被系统在激活变化时立即关闭
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

	// 每次弹出前重建：动作行（不可用时置灰）+ 一句原因
	void build(bool canRemove, const QString& blockedReason)
	{
		clear();
		m_chosen = false;

		auto* remove = new QPushButton(qtTrId("common_delete"), m_body);
		remove->setObjectName(QStringLiteral("modelListContextAction"));
		remove->setFlat(true);
		remove->setCursor(Qt::PointingHandCursor);
		// 菜单行不参与焦点链：弹出层一旦出现可聚焦子控件，Windows 上会因激活变化被关掉
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

	// 用户是否点了「删除」
	bool removeChosen() const { return m_chosen; }

private:
	void clear()
	{
		// 与模型选择器菜单同理：只 deleteLater() 的话旧行在真正销毁前仍是子控件，
		// 下次 show() 会被一并显示出来——这里立刻从控件树上摘下来。
		while (QLayoutItem* item = m_rows->takeAt(0)) {
			if (QWidget* widget = item->widget()) {
				widget->hide();
				widget->setParent(nullptr);
				widget->deleteLater();
			}
			delete item;
		}
	}

	QWidget* m_body = nullptr;
	QVBoxLayout* m_rows = nullptr;
	bool m_chosen = false;
};

// ------------------------------------------------------------------
// ModelListPanel
// ------------------------------------------------------------------

ModelListPanel::ModelListPanel(DshApiClient* api, QWidget* parent)
	: QWidget(parent)
	, m_api(api)
{
	setObjectName(QStringLiteral("modelListPanel"));

	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	// 顶部三行说明文字之间贴紧些（原来 6px 偏松）
	layout->setSpacing(3);

	auto* title = new QLabel(qtTrId("model_list_title"), this);
	title->setObjectName(QStringLiteral("modelListTitle"));
	// 纵向 Fixed：这些说明文字只占自己一行的高度，不被布局拉长
	title->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

	auto* hint = new QLabel(
		qtTrId("model_list_desc"),
		this);
	hint->setWordWrap(true);
	hint->setObjectName(QStringLiteral("modelListHint"));

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	m_status->setObjectName(QStringLiteral("modelListStatus"));
	// 状态行只有一行字：别让它被拉成一大块（看起来“上下很松”）
	m_status->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

	// 列表级操作的瞬时提示（右键删除那类不在表单里的操作）：默认隐藏，几秒后自己消失
	m_notice = new QLabel(this);
	m_notice->setWordWrap(true);
	m_notice->setObjectName(QStringLiteral("modelListNotice"));
	m_notice->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	m_notice->hide();

	// ---------------- 列表 ----------------
	m_scroll = new QScrollArea(this);
	m_scroll->setObjectName(QStringLiteral("modelListScroll"));
	m_scroll->setFrameShape(QFrame::NoFrame);
	m_scroll->setWidgetResizable(true);
	m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	// 清单区占满面板剩余空间：装得下就不出滚动条，装不下才滚动。
	// 不按内容设固定高度——那会把窗口的最小高度一起顶大。
	m_scroll->setMinimumHeight(kMinListHeight);
	Theme::repolishScrollArea(m_scroll);

	m_listContent = new QWidget(m_scroll);
	m_listContent->setObjectName(QStringLiteral("modelListContent"));

	// 外层：模型行容器 + 新增表单。表单放在滚动区里，所以它撑高的是滚动内容，
	// 不是窗口——窗口高度不变，“添加模型”按钮也就不会跟着表单上下移动。
	auto* contentLayout = new QVBoxLayout(m_listContent);
	contentLayout->setContentsMargins(0, 0, 0, 0);
	contentLayout->setSpacing(4);

	m_rowsHost = new QWidget(m_listContent);
	m_rowsHost->setObjectName(QStringLiteral("modelListRows"));
	m_listLayout = new QVBoxLayout(m_rowsHost);
	m_listLayout->setContentsMargins(0, 0, 0, 0);
	// 成员之间贴紧些：每行自身已经有两行文字，行距再大就散
	m_listLayout->setSpacing(2);
	m_listLayout->addStretch(1);
	contentLayout->addWidget(m_rowsHost);

	buildForm(m_listContent);
	contentLayout->addWidget(m_form);

	m_scroll->setWidget(m_listContent);

	// ---------------- 添加模型 ----------------
	m_addButton = new QPushButton(qtTrId("model_add"), this);
	m_addButton->setObjectName(QStringLiteral("modelListAddButton"));
	m_addButton->setCursor(Qt::PointingHandCursor);

	layout->addWidget(title);
	layout->addWidget(hint);
	layout->addWidget(m_status);
	layout->addWidget(m_notice);
	layout->addWidget(m_scroll, 1);
	layout->addWidget(m_addButton);

	connect(m_addButton, &QPushButton::clicked, this, [this]() {
		if (m_form && m_form->isVisible())
			closeForm();
		else
			openForm();
		});
}

// ------------------------------------------------------------------
// 拉取与重建
// ------------------------------------------------------------------

void ModelListPanel::refresh()
{
	if (!m_api) {
		if (m_status)
			m_status->setText(qtTrId("model_server_not_ready"));
		return;
	}

	if (m_status)
		m_status->setText(qtTrId("model_loading_catalog"));

	// 面板是常驻对象，请求可能在关闭设置后才回来
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

	// 表单里的提供方下拉跟随最新目录重建；正在提交时不动，免得把用户选择清掉
	if (!m_submitting)
		syncFormToRoute();
}

void ModelListPanel::clearRows()
{
	if (!m_listLayout)
		return;

	// 只 deleteLater() 的话旧行在真正销毁前仍是子控件，会被一并显示出来，
	// 于是列表里出现重复成员——这里立刻从控件树上摘下来。
	while (QLayoutItem* item = m_listLayout->takeAt(0)) {
		if (QWidget* widget = item->widget()) {
			widget->hide();
			widget->setParent(nullptr);
			widget->deleteLater();
		}
		delete item;
	}
}

void ModelListPanel::populateRows()
{
	if (!m_listLayout)
		return;

	clearRows();

	QString lastProvider;
	for (const ModelInfo& info : m_rows) {
		// 提供方之间插一条分隔标题，多提供方时列表才读得下去
		if (info.provider != lastProvider) {
			lastProvider = info.provider;

			auto* header = new QLabel(info.providerName, m_listContent);
			header->setObjectName(QStringLiteral("modelListGroup"));
			m_listLayout->addWidget(header);
		}

		auto* entry = new ModelListEntry(info, m_listContent);
		// 刷新前展开着的成员，重建后保持展开
		if (m_expanded.contains(entry->key()))
			entry->setExpanded(true);

		connect(entry, &ModelListEntry::expandedChanged, this,
			[this](const QString& key, bool expanded) {
				if (expanded)
					m_expanded.insert(key);
				else
					m_expanded.remove(key);
			});

		// 右键出菜单（目前只有删除）
		connect(entry, &ModelListEntry::contextMenuRequested,
			this, &ModelListPanel::showRowMenu);

		m_listLayout->addWidget(entry);
	}

	if (m_rows.isEmpty()) {
		auto* empty = new QLabel(
			qtTrId("model_catalog_empty"),
			m_listContent);
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
	notes.append(qtTrId("model_catalog_summary_fmt")
		.arg(m_rows.size())
		.arg(m_view.groups.size()));

	// 写不了就直说，别让用户填完表单才被拒
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
		m_addButton->setToolTip(canWrite
			? QString()
			: qtTrId("model_add_requires_api"));
	}
}

// ------------------------------------------------------------------
// 添加模型的表单
// ------------------------------------------------------------------

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
	addField(qtTrId("model_id_label"), m_idEdit);

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

	// 下面是按适配器族二选一的字段，切路由时整对显隐
	// （QGridLayout 的行只含隐藏控件时高度归零，所以直接隐藏标签与控件即可）
	m_effortsLabel = new QLabel(qtTrId("model_think_level"), m_form);
	m_effortsLabel->setObjectName(QStringLiteral("modelListFieldLabel"));
	grid->addWidget(m_effortsLabel, row, 0, Qt::AlignRight | Qt::AlignVCenter);

	m_effortsEdit = new QLineEdit(m_form);
	m_effortsEdit->setObjectName(QStringLiteral("modelListField"));
	m_effortsEdit->setPlaceholderText(
		qtTrId("model_think_level_hint"));
	grid->addWidget(m_effortsEdit, row, 1);
	++row;

	m_modalitiesLabel = new QLabel(qtTrId("model_input_modality"), m_form);
	m_modalitiesLabel->setObjectName(QStringLiteral("modelListFieldLabel"));
	grid->addWidget(m_modalitiesLabel, row, 0, Qt::AlignRight | Qt::AlignVCenter);

	m_imageInputBox = new QCheckBox(qtTrId("model_supports_image"), m_form);
	m_imageInputBox->setObjectName(QStringLiteral("modelListField"));
	grid->addWidget(m_imageInputBox, row, 1);
	++row;

	// API Key：凭据也写在服务端，按该路由 profile 的 apiKeyEnv 引用存取。
	// 填了才会写，留空表示不动现有凭据（例如用环境变量或已配好的 key）。
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

	// 引用名与状态提示（放在字段下方，跟随所选路由刷新）
	m_apiKeyRefLabel = new QLabel(m_form);
	m_apiKeyRefLabel->setWordWrap(true);
	m_apiKeyRefLabel->setObjectName(QStringLiteral("modelListApiKeyRef"));
	layout->addWidget(m_apiKeyRefLabel);

	m_routeHint = new QLabel(m_form);
	m_routeHint->setWordWrap(true);
	m_routeHint->setObjectName(QStringLiteral("modelListRouteHint"));
	layout->addWidget(m_routeHint);

	m_feedback = new QLabel(m_form);
	m_feedback->setWordWrap(true);
	m_feedback->setObjectName(QStringLiteral("modelListFeedback"));
	m_feedback->hide();
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

	connect(cancelButton, &QPushButton::clicked, this, [this]() { closeForm(); });
	connect(m_submitButton, &QPushButton::clicked, this, [this]() { submitForm(); });

	// 路由换了，提示文案与按族显示的字段都要跟着换
	connect(m_providerCombo, &QComboBox::currentIndexChanged, this, [this](int) {
		if (m_submitting)
			return;
		syncFormToRoute();
		});
}

void ModelListPanel::setFeedback(const QString& text)
{
	m_feedback->setText(text);
	m_feedback->show();
}

void ModelListPanel::clearFeedback()
{
	m_feedback->hide();
}

void ModelListPanel::setNotice(const QString& text)
{
	m_notice->setText(text);
	m_notice->show();

	// 几秒后自己消失，免得旧提示一直挂着
	QTimer::singleShot(4000, this, [this]() {
		if (m_notice)
			m_notice->hide();
		});
}

void ModelListPanel::showRowMenu(
	const QString& provider, const QString& modelId, const QPoint& globalPos)
{
	if (provider.isEmpty() || modelId.isEmpty())
		return;

	if (!m_rowMenu)
		m_rowMenu = new ContextMenu(this);

	// 写不了服务端就没法删，直接把原因写在菜单里
	const QString blocked = m_view.settingsWritable
		? QString()
		: qtTrId("model_delete_readonly");
	m_rowMenu->build(m_view.settingsWritable, blocked);
	m_rowMenu->adjustSize();

	// 夹到屏幕内，避免贴着边缘弹出时被切掉
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

	// 复制一份供异步回调用（provider/ns 指向 m_view 内部，刷新后就失效了）
	const ConfigurableProvider providerCopy = *entry;
	const SettingsNamespace nsCopy = *ns;
	const QString settingsNs = providerCopy.settingsNs;

	QPointer<ModelListPanel> self(this);

	ModelSelectionService::removeModel(m_api, providerCopy, nsCopy, modelId,
		[self, settingsNs, provider, modelId](const SettingsNamespace& updated) {
			if (!self)
				return;

			// 用回包的新视图就地更新，省一次 describe 往返
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

			// 目录是适配器侧的：删掉的条目要等 settings 热生效后才会从 session/modelCatalog 消失，
			// 所以再拉一次以服务端事实为准。
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

	// 表单在滚动区底部：拉出来之后要滚到它那儿，否则用户只看到列表变短了
	if (m_scroll && m_form) {
		// 等布局把新高度算完再滚，否则滚到的是旧的底部
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
		parts.append(qtTrId("model_namespace_missing_fmt")
			.arg(provider.settingsNs));
		return parts.join(QLatin1Char(' '));
	}

	const QJsonArray models = ModelSelectionService::configuredModels(*ns, provider.settingsPath);
	const bool userDeclared = ModelSelectionService::userDeclaresModels(*ns, provider.settingsPath);

	parts.append(qtTrId("model_write_progress_fmt")
		.arg(provider.settingsNs,
			ModelSelectionService::modelsPath(provider).join(QLatin1Char('.')),
			QString::number(models.size())));

	if (provider.settingsNs.contains(QStringLiteral("pi-ai"))) {
		// pi-ai 的 models 是整体替换：路由自带适配器随附目录时，
		// 第一次写成显式列表会收窄公布范围（随附的其余模型不再出现）。
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

	// 保留下拉里已经选中的路由，重建条目时不丢用户选择
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
		m_routeHint->setText(provider
			? routeHint(*provider)
			: qtTrId("model_no_configurable_provider"));
	}

	refreshCredentialRow();

	if (m_submitButton)
		m_submitButton->setEnabled(provider != nullptr && m_view.settingsWritable);
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

	// 引用名：profile 点名的优先，没有就按 harness 约定派生
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

	// 服务端才是凭据状态的权威：问一次该引用是否已配置、是否可写
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
				return; // 已经切到别的路由，丢弃过期结果

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
			// 问不到状态不阻塞填写：按“可写、未知是否已配置”处理
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

	// 只有服务端确实回报了该引用，才敢下“只读/已配置”的结论。
	// 之前这里用 writable 判断，而 CredentialStatus 的默认值就是 false，
	// 于是“还没查回来”被当成“只读”，占位符写进去后再没还原过。
	const bool readOnly = m_credential.known && !m_credential.writable;

	QString state;
	if (!m_credential.known) {
		state = qtTrId("model_credential_unknown_suffix");
	}
	else if (!m_credential.writable) {
		// 部署把该引用交给环境变量管理：表单不该假装能改它
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
		setFeedback(qtTrId("model_write_namespace_missing_fmt")
			.arg(provider->settingsNs));
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

	// pi-ai：把逗号分隔的档位名解析并校验；空串 = 不声明（沿用同 id 已安装条目）
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

	// 凭据随这次新增一起处理：profile 点名的引用优先，没有就按约定派生并记入配置
	request.apiKeyRef = m_keyRef;
	request.recordApiKeyEnv = !m_keyRef.isEmpty()
		&& ModelSelectionService::profileApiKeyEnv(*ns, provider->settingsPath).isEmpty();

	const QString apiKey = m_apiKeyEdit ? m_apiKeyEdit->text().trimmed() : QString();

	m_submitting = true;
	if (m_submitButton)
		m_submitButton->setEnabled(false);
	setFeedback(apiKey.isEmpty()
		? qtTrId("model_writing")
		: qtTrId("model_writing_credential"));

	// 按提供方路由复制一份，供异步回调使用（provider/ns 指向 m_view 内部，刷新会失效）
	const ConfigurableProvider providerCopy = *provider;
	const SettingsNamespace nsCopy = *ns;

	if (apiKey.isEmpty()) {
		writeModel(providerCopy, nsCopy, request);
		return;
	}

	// 先写凭据再写模型：key 写失败就不要把模型落下去，
	// 否则会得到一条指向未配置凭据的模型（能选中、一发请求就报缺凭据）。
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

	// 同 id 已在这份列表里 -> 这次是“原地覆盖”而不是新增，
	// 反馈文案与日志都按这个区分（覆盖时条目字段以表单为准）。
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
			self->m_credential = CredentialStatus();   // key 变了，状态待重查

			// 用回包的新视图就地更新，省一次 describe 往返
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

			// 目录是适配器侧的：刚写下的条目要等 settings 热生效后才会出现在
			// session/modelCatalog 里，所以再拉一次以服务端事实为准。
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