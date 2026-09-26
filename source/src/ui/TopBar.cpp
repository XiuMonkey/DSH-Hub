#include "ui/TopBar.h"

#include "ui/LayoutUtils.h"
#include "common/appearance/CardShadow.h"
#include "common/appearance/ThemeManager.h"
#include "common/appearance/WindowFrame.h"
#include "common/util/CommonRegistry.h"
#include "core/ConnectionManager.h"
#include "core/HostExports.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QDebug>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>
#include <utility>
#include <cmath>

namespace
{
	// 顶栏右侧"工具过滤"按钮：三条递减胶囊，在 paintEvent 里矢量绘制。不用 QIcon+QPixmap：
	// 位图要按 devicePixelRatio 预生成、几何不落在整像素上会发虚（实测一个满覆盖像素都没有）；
	// 颜色每次现取 ThemeManager，不缓存 —— 主题靠重建窗口切换，这样不会忘了跟着换色。
	class ToolsFilterButton : public QPushButton
	{
	public:
		explicit ToolsFilterButton(QWidget* parent = nullptr)
			: QPushButton(parent)
		{
			setObjectName(QStringLiteral("topBarToolsButton"));
			setFixedSize(32, 32);
			setCursor(Qt::PointingHandCursor);
		}

	protected:
		void paintEvent(QPaintEvent* event) override
		{
			// 先让 QSS 画底（透明 / hover / pressed），再把图标叠上去
			QPushButton::paintEvent(event);

			// 20×20 设计栅格换算到设备像素后取整：线的上下边落在像素边界上，不靠抗锯齿"猜"
			const qreal dpr = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
			const auto dev = [dpr](qreal value) { return std::round(value * dpr); };

			QPainter painter(this);
			painter.setRenderHint(QPainter::Antialiasing, true); // 只有胶囊两端的圆头需要
			painter.setPen(Qt::NoPen);
			painter.setBrush(CardShadow::parseColor(ThemeManager::instance().textSecondary()));
			painter.scale(1.0 / dpr, 1.0 / dpr); // 之后 1 单位 = 1 设备像素

			const qreal grid = dev(20.0);
			const qreal originX = std::round((width() * dpr - grid) / 2.0);
			const qreal originY = std::round((height() * dpr - grid) / 2.0);

			const auto bar = [&](qreal x1, qreal x2, qreal y) {
				const QRectF rect(originX + dev(x1), originY + dev(y), dev(x2) - dev(x1), dev(2.0));
				painter.drawRoundedRect(rect, rect.height() / 2.0, rect.height() / 2.0);
			};
			// 三根都以 x=10 居中：宽度 14 / 10 / 4
			bar(3.0, 17.0, 6.0);
			bar(5.0, 15.0, 10.0);
			bar(8.0, 12.0, 14.0);
		}
	};

	// 目录底部的整组「折叠/可见」开关：整行可点，左状态文案、右胶囊滑块。刻意不用 QCheckBox
	//（::indicator 只能换底色、画不出滑块）；配色每次现取 ThemeManager，理由同 ToolsFilterButton。
	class CapsuleSwitchRow : public QAbstractButton
	{
	public:
		explicit CapsuleSwitchRow(QWidget* parent = nullptr)
			: QAbstractButton(parent)
		{
			setCheckable(true);
			setCursor(Qt::PointingHandCursor);
			setFocusPolicy(Qt::NoFocus);
			setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		}

		QSize sizeHint() const override
		{
			return QSize(160, 26);
		}

	protected:
		void paintEvent(QPaintEvent* event) override
		{
			Q_UNUSED(event);

			QPainter painter(this);
			painter.setRenderHint(QPainter::Antialiasing, true);

			// 左侧状态文案：常态次级灰、悬停提亮一档、禁用再淡一档
			QFont font = this->font();
			font.setPixelSize(12);
			painter.setFont(font);
			const QColor textColor = isEnabled()
				? CardShadow::parseColor(underMouse()
					? ThemeManager::instance().textSecondary()
					: ThemeManager::instance().color(QStringLiteral("textTertiary")))
				: CardShadow::parseColor(ThemeManager::instance().color(
					QStringLiteral("textCaption")));
			painter.setPen(textColor);
			const int textWidth = qMax(width() - int(kTrackWidth) - 14, 0);
			painter.drawText(QRect(4, 0, textWidth, height()), Qt::AlignVCenter | Qt::AlignLeft, text());

			// 右侧胶囊：勾选（折叠）填 textSecondary（与工具行勾选框同款，不用主题色 accent），
			// 滑块滑到右端；未勾选浅灰轨，滑块在左端
			const qreal trackX = width() - kTrackWidth - 4.0;
			const qreal trackY = (height() - kTrackHeight) / 2.0;
			const QRectF track(trackX, trackY, kTrackWidth, kTrackHeight);
			painter.setPen(Qt::NoPen);
			painter.setBrush(CardShadow::parseColor(isEnabled()
				? (isChecked() ? ThemeManager::instance().textSecondary() : ThemeManager::instance().border())
				: ThemeManager::instance().inputBg()));
			painter.drawRoundedRect(track, kTrackHeight / 2.0, kTrackHeight / 2.0);

			// 滑块：白色圆点在灰轨与主题色轨上都清晰（两套主题的惯例同此）
			const qreal knobSize = kTrackHeight - 4.0;
			const qreal knobX = isChecked()
				? trackX + kTrackWidth - knobSize - 2.0
				: trackX + 2.0;
			painter.setBrush(isEnabled() ? QColor(Qt::white)
				: CardShadow::parseColor(ThemeManager::instance().border()));
			painter.drawEllipse(QPointF(knobX + knobSize / 2.0, trackY + kTrackHeight / 2.0),
				knobSize / 2.0, knobSize / 2.0);
		}

	private:
		// 胶囊轨道的几何（逻辑像素；矢量绘制，高分辨率下不发虚）
		static constexpr qreal kTrackWidth = 34.0;
		static constexpr qreal kTrackHeight = 18.0;
	};

	// 目录表头的箭头，字形与 ModelListEntry 成员行同一套（U+25BE / U+25B8）。
	// 用 QChar(码点) 而非字面量：QLatin1String("\u25BE") 会把三个字节逐字节当成 Latin-1（界面出乱码），
	// 直接写窄字面量又依赖源文件被当成 UTF-8 读；QChar(0x25BE) 源码全 ASCII，与编码无关。
	const QString kChevronExpanded(QChar(0x25BE));
	const QString kChevronCollapsed(QChar(0x25B8));

	// 目录表头的最小高度：两行文字（目录名 + 计数）+ 上下内边距。不给下限时 QPushButton 按
	// "一行文本 + 按钮内边距"算高度，第二行会被压掉（ModelListEntry 踩过同一个坑）。
	constexpr int kDirectoryHeaderMinHeight = 44;
	constexpr int kDirectoryHeaderPadding = 6;
} // namespace

ToolsFilterDirectoryEntry::ToolsFilterDirectoryEntry(const ToolFilterDirectory& directory, QWidget* parent)
	: QWidget(parent)
	, m_name(directory.name)
	, m_description(directory.description)
	, m_groupHidden(!directory.expanded)
{
	setObjectName(QStringLiteral("toolsFilterDirectory"));
	setAttribute(Qt::WA_StyledBackground, true);

	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	// 表头就是一枚按钮：整行可点，语义与列表项一致
	m_header = new QPushButton(this);
	m_header->setObjectName(QStringLiteral("toolsFilterDirectoryHeader"));
	m_header->setFlat(true);
	m_header->setCursor(Qt::PointingHandCursor);
	m_header->setFocusPolicy(Qt::NoFocus);
	m_header->setMinimumHeight(kDirectoryHeaderMinHeight);

	auto* headerLayout = new QHBoxLayout(m_header);
	headerLayout->setContentsMargins(10, kDirectoryHeaderPadding, 10, kDirectoryHeaderPadding);
	headerLayout->setSpacing(8);

	m_chevron = new QLabel(kChevronExpanded, m_header);
	m_chevron->setObjectName(QStringLiteral("toolsFilterDirectoryChevron"));
	m_chevron->setAlignment(Qt::AlignCenter);
	headerLayout->addWidget(m_chevron, 0, Qt::AlignVCenter);

	m_nameLabel = new QLabel(m_name, m_header);
	m_nameLabel->setObjectName(QStringLiteral("toolsFilterDirectoryName"));
	m_nameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
	headerLayout->addWidget(m_nameLabel, 1, Qt::AlignVCenter);

	m_metaLabel = new QLabel(m_header);
	m_metaLabel->setObjectName(QStringLiteral("toolsFilterDirectoryMeta"));
	m_metaLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
	headerLayout->addWidget(m_metaLabel, 0, Qt::AlignVCenter);

	layout->addWidget(m_header);

	m_body = new QWidget(this);
	m_body->setObjectName(QStringLiteral("toolsFilterDirectoryBody"));

	auto* bodyLayout = new QVBoxLayout(m_body);
	bodyLayout->setContentsMargins(10, 0, 10, 8);
	bodyLayout->setSpacing(2);

	if (directory.tools.isEmpty()) {
		// 空目录也要给一句提示：否则展开后一片空白，看起来像"点坏了"
		auto* empty = new QLabel(qtTrId("toolfilter_dir_empty"), m_body);
		empty->setObjectName(QStringLiteral("toolsFilterDirectoryEmpty"));
		empty->setWordWrap(true);
		bodyLayout->addWidget(empty);
	}

	for (int index = 0; index < directory.tools.size(); ++index) {
		const ToolFilterEntry& tool = directory.tools.at(index);

		auto* box = new QCheckBox(tool.name, m_body);
		box->setObjectName(QStringLiteral("toolsFilterToolRow"));
		box->setCursor(Qt::PointingHandCursor);
		// 先设好初始勾选状态再接线：否则重建列表会被当成"用户改了勾选"写回配置
		box->setChecked(tool.visible);

		// 描述与参数只在这里出现（不写进配置文件）
		QString tip = tool.description;
		if (!tool.parameters.isEmpty() && tool.parameters != QStringLiteral("{}"))
			tip += QStringLiteral("\n\n%1 %2").arg(qtTrId("toolfilter_params_label"), tool.parameters);
		if (!tip.isEmpty())
			box->setToolTip(tip);

		dshRegister(QStringLiteral("TopBar.tool.%1.%2").arg(m_name).arg(index), box,
			qOverload<bool>(&QCheckBox::toggled), this, [this, index](bool checked) {
				refreshMeta();
				emit toolVisibilityChanged(m_name, m_toolNames.at(index), checked);
			});

		m_boxes.append(box);
		m_toolNames.append(tool.name);
		bodyLayout->addWidget(box);
	}

	// 底部开关：这一目录的整组「折叠/可见」，写的是配置里该目录的 IsExpanded（折叠 = 插件把整组
	// 从模型清单撤下），与表头那个纯显示的展开/收起不同。先设状态再接线。
	m_groupToggle = new CapsuleSwitchRow(m_body);
	m_groupToggle->setObjectName(QStringLiteral("toolsFilterGroupToggle"));
	m_groupToggle->setChecked(m_groupHidden);
	dshRegister(QStringLiteral("TopBar.group.%1").arg(m_name), m_groupToggle,
		qOverload<bool>(&QAbstractButton::toggled), this, [this](bool collapsed) {
			m_groupHidden = collapsed;
			refreshMeta();
			applyGroupHiddenVisuals();
			emit groupHiddenChanged(m_name, collapsed);
		});
	bodyLayout->addSpacing(4);
	bodyLayout->addWidget(m_groupToggle);

	layout->addWidget(m_body);

	refreshMeta();
	applyGroupHiddenVisuals();
	setExpanded(true);

	dshRegister(QStringLiteral("TopBar.header.%1").arg(m_name), m_header,
		qOverload<bool>(&QPushButton::clicked), this, [this]() {
			setExpanded(!isExpanded());
		});
}

bool ToolsFilterDirectoryEntry::isExpanded() const
{
	return m_expanded;
}

void ToolsFilterDirectoryEntry::setExpanded(bool expanded)
{
	if (!m_body)
		return;

	m_expanded = expanded;
	m_body->setVisible(expanded);
	if (m_chevron)
		m_chevron->setText(expanded ? kChevronExpanded : kChevronCollapsed);

	emit expandedChanged(m_name, expanded);
}

void ToolsFilterDirectoryEntry::refreshMeta()
{
	if (!m_metaLabel)
		return;

	int hidden = 0;
	for (const QCheckBox* box : m_boxes) {
		if (box->checkState() != Qt::Checked)
			++hidden;
	}

	QString text = m_boxes.isEmpty()
		? qtTrId("toolfilter_no_tools")
		: qtTrId("toolfilter_summary_fmt").arg(m_boxes.size()).arg(hidden);
	// 配置里写了 IsExpanded:"False"（整组隐藏）时要说清楚：那时下面的勾选其实不起作用
	if (m_groupHidden)
		text += qtTrId("toolfilter_group_hidden_suffix");
	m_metaLabel->setText(text);

	if (m_header) {
		QString tip = m_description;
		if (!tip.isEmpty())
			tip += QStringLiteral("\n\n");
		tip += qtTrId("toolfilter_dir_toggle_tip");
		m_header->setToolTip(tip);
	}
}

void ToolsFilterDirectoryEntry::applyGroupHiddenVisuals()
{
	if (m_groupToggle) {
		m_groupToggle->setText(m_groupHidden
			? qtTrId("toolfilter_group_state_collapsed")
			: qtTrId("toolfilter_group_state_visible"));
		m_groupToggle->setToolTip(qtTrId("toolfilter_group_toggle_tip"));
	}
	// 整组隐藏时各行勾选其实不生效：把勾选框禁用，别让界面撒谎
	for (QCheckBox* box : m_boxes)
		box->setEnabled(!m_groupHidden);
}

ToolsFilterPopup::ToolsFilterPopup(QWidget* parent)
	: StatusPopupWindow(parent)
{
	setObjectName(QStringLiteral("toolsFilterPopup"));
	setTitle(qtTrId("toolfilter_title"));
	setFixedSize(460, 560);

	auto* content = new QWidget(this);
	auto* layout = new QVBoxLayout(content);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(10);

	m_hint = new QLabel(content);
	m_hint->setObjectName(QStringLiteral("toolsFilterHint"));
	m_hint->setWordWrap(true);
	layout->addWidget(m_hint);

	// 列表区：一个可滚动的容器，里面每个目录是一行（表头按钮 + 工具行）
	m_scroll = LayoutUtils::makeThemedScrollArea(content, QStringLiteral("toolsFilterScroll"));
	layout->addWidget(m_scroll, 1);

	m_listContent = new QWidget(m_scroll);
	m_listContent->setObjectName(QStringLiteral("toolsFilterListContent"));
	m_listLayout = new QVBoxLayout(m_listContent);
	m_listLayout->setContentsMargins(0, 0, 0, 0);
	m_listLayout->setSpacing(6);
	m_listLayout->addStretch(1);
	m_scroll->setWidget(m_listContent);

	auto* buttonRow = new QHBoxLayout;
	buttonRow->setContentsMargins(0, 0, 0, 0);
	buttonRow->setSpacing(8);

	m_showAllButton = new QPushButton(qtTrId("toolfilter_show_all"), content);
	m_showAllButton->setObjectName(QStringLiteral("toolsFilterShowAllButton"));
	m_showAllButton->setCursor(Qt::PointingHandCursor);

	m_hideAllButton = new QPushButton(qtTrId("toolfilter_hide_all"), content);
	m_hideAllButton->setObjectName(QStringLiteral("toolsFilterHideAllButton"));
	m_hideAllButton->setCursor(Qt::PointingHandCursor);

	m_refreshButton = new QPushButton(qtTrId("common_refresh"), content);
	m_refreshButton->setObjectName(QStringLiteral("toolsFilterRefreshButton"));
	m_refreshButton->setCursor(Qt::PointingHandCursor);

	buttonRow->addWidget(m_showAllButton);
	buttonRow->addWidget(m_hideAllButton);
	buttonRow->addStretch();
	buttonRow->addWidget(m_refreshButton);
	layout->addLayout(buttonRow);

	m_statusLabel = new QLabel(content);
	m_statusLabel->setObjectName(QStringLiteral("toolsFilterStatus"));
	layout->addWidget(m_statusLabel);
	// 让状态文案按当前真实宽度两行省略（与插件/扩展窗口同款）
	attachStatusLabel(m_statusLabel);

	setContent(content);

	dshRegister("TopBar.001", m_showAllButton,
		qOverload<bool>(&QPushButton::clicked), this, [this]() { setAllVisible(true); });
	dshRegister("TopBar.002", m_hideAllButton,
		qOverload<bool>(&QPushButton::clicked), this, [this]() { setAllVisible(false); });
	dshRegister("TopBar.003", m_refreshButton,
		qOverload<bool>(&QPushButton::clicked), this, [this]() { emit refreshRequested(); });

	retranslateStaticText();
	updateStatus();
}

void ToolsFilterPopup::retranslateStaticText()
{
	if (m_hint) {
		m_hint->setText(qtTrId("toolfilter_help_desc"));
	}
	if (m_showAllButton)
		m_showAllButton->setText(qtTrId("toolfilter_show_all"));
	if (m_hideAllButton)
		m_hideAllButton->setText(qtTrId("toolfilter_hide_all"));
	if (m_refreshButton)
		m_refreshButton->setText(qtTrId("common_refresh"));
}

void ToolsFilterPopup::setContext(ToolsFilter* filter, const QString& sessionId, bool dropGuidance,
	const QStringList& hideContexts)
{
	m_filter = filter;
	m_sessionId = sessionId;
	m_dropGuidance = dropGuidance;
	m_hideContexts = hideContexts;
}

void ToolsFilterPopup::setBusy(bool busy)
{
	if (busy)
		setStatus(qtTrId("toolfilter_loading"));
}

void ToolsFilterPopup::applyCatalog(const ToolFilterCatalog& catalog)
{
	if (!catalog.ok) {
		m_directories.clear();
		rebuild();
		m_degraded = false;
		setStatus(catalog.error.isEmpty() ? qtTrId("toolfilter_load_failed") : catalog.error);
		return;
	}

	// 新会话默认全折叠：把本次目录名记进"收起"集，用户随后的展开/收起照常记录，重开窗口不丢。
	// ⚠️ 判断"新会话"不能用 m_sessionId —— loadTools 成功路径会先 setContext() 把它覆盖成本会话，
	// 再比较永远相等（"换会话清空"从未生效的原因），所以单独记一个已播种的会话号。
	if (m_collapsedSeedSession != catalog.sessionId) {
		m_collapsed.clear();
		for (const ToolFilterDirectory& directory : catalog.directories)
			m_collapsed.insert(directory.name);
		m_collapsedSeedSession = catalog.sessionId;
	}

	m_sessionId = catalog.sessionId;
	m_dropGuidance = catalog.dropGuidance;
	m_hideContexts = catalog.hideContexts;
	m_degraded = catalog.degraded;
	m_directories = catalog.directories;
	rebuild();
	updateStatus();
}

void ToolsFilterPopup::rebuild()
{
	if (!m_listLayout)
		return;

	m_updating = true;

	// 重建后马上 show()，所以要立刻摘离 —— 否则列表里会出现重复目录。
	LayoutUtils::clearLayout(m_listLayout);

	for (const ToolFilterDirectory& directory : m_directories) {
		auto* entry = new ToolsFilterDirectoryEntry(directory, m_listContent);
		// 先摆好展开状态再接线：setExpanded() 会发信号，接线在前等于自己写回自己
		entry->setExpanded(!m_collapsed.contains(directory.name));

		// entry 每次 rebuild 都重建，序号不唯一，不进登记表
		connect(entry, &ToolsFilterDirectoryEntry::expandedChanged,
			this, &ToolsFilterPopup::onDirectoryExpandedChanged);
		connect(entry, &ToolsFilterDirectoryEntry::toolVisibilityChanged,
			this, &ToolsFilterPopup::onToolVisibilityChanged);
		connect(entry, &ToolsFilterDirectoryEntry::groupHiddenChanged,
			this, &ToolsFilterPopup::onDirectoryGroupHiddenChanged);

		m_listLayout->addWidget(entry);
	}

	if (m_directories.isEmpty()) {
		auto* empty = new QLabel(qtTrId("toolfilter_no_filterable_tools"), m_listContent);
		empty->setObjectName(QStringLiteral("toolsFilterEmpty"));
		empty->setWordWrap(true);
		m_listLayout->addWidget(empty);
	}

	m_listLayout->addStretch(1);

	m_updating = false;
}

QVector<ToolFilterDirectory> ToolsFilterPopup::collectDirectories() const
{
	return m_directories;
}

void ToolsFilterPopup::onDirectoryExpandedChanged(const QString& directoryName, bool expanded)
{
	// 只记本次翻看的显示状态，不写回配置：配置里的 IsExpanded 是筛选语义（"False" = 整组隐藏）
	if (m_updating)
		return;

	if (expanded)
		m_collapsed.remove(directoryName);
	else
		m_collapsed.insert(directoryName);
}

void ToolsFilterPopup::onToolVisibilityChanged(const QString& directoryName, const QString& toolName,
	bool visible)
{
	if (m_updating)
		return;

	for (ToolFilterDirectory& directory : m_directories) {
		if (directory.name != directoryName)
			continue;
		for (ToolFilterEntry& tool : directory.tools) {
			if (tool.name != toolName)
				continue;
			if (tool.visible == visible)
				return;
			tool.visible = visible;
			saveNow();
			updateStatus();
			return;
		}
		return;
	}
}

void ToolsFilterPopup::onDirectoryGroupHiddenChanged(const QString& directoryName, bool collapsed)
{
	if (m_updating)
		return;

	for (ToolFilterDirectory& directory : m_directories) {
		if (directory.name != directoryName)
			continue;
		if (directory.expanded == !collapsed)
			return; // 状态没变
		// 这里写的就是配置里该目录的 IsExpanded（"False" = 整组隐藏），与表头那个纯显示的
		// 展开/收起不同。条目已自行更新后缀与可用性，这里只改模型 + 写回，不必重建。
		directory.expanded = !collapsed;
		saveNow();
		updateStatus();
		return;
	}
}

void ToolsFilterPopup::setAllVisible(bool visible)
{
	if (m_directories.isEmpty())
		return;

	for (ToolFilterDirectory& directory : m_directories) {
		for (ToolFilterEntry& tool : directory.tools)
			tool.visible = visible;
	}

	// 重建而不是逐个改勾选框：逐个改会触发一串 toggled，写回一次配置就够了
	rebuild();
	saveNow();
	updateStatus();
}

void ToolsFilterPopup::saveNow()
{
	if (!m_filter || m_sessionId.isEmpty()) {
		setStatus(qtTrId("toolfilter_no_session"));
		return;
	}

	setStatus(qtTrId("toolfilter_saving"));
	const QString sessionId = m_sessionId;
	const QVector<ToolFilterDirectory> directories = collectDirectories();
	const bool dropGuidance = m_dropGuidance;
	const QStringList hideContexts = m_hideContexts;

	// 异步写回：回调里再更新状态，绝不阻塞界面
	m_filter->save(sessionId, directories, dropGuidance, hideContexts,
		[this, sessionId](bool ok, const QString& error) {
			if (sessionId != m_sessionId)
				return; // 会话已经切走，这次结果作废
			if (!ok) {
				setStatus(error.isEmpty() ? qtTrId("toolfilter_save_failed") : error);
				return;
			}
			updateStatus();
		});
}

void ToolsFilterPopup::updateStatus()
{
	const int count = ToolsFilter::toolCount(m_directories);
	if (count == 0) {
		setStatus(qtTrId("toolfilter_no_filterable_tools"));
		return;
	}

	QString text = qtTrId("toolfilter_summary_full_fmt").arg(count).arg(m_directories.size())
		.arg(ToolsFilter::hiddenCount(m_directories));
	if (m_degraded)
		text += qtTrId("toolfilter_global_layer_only_suffix");
	setStatus(text);
}

void ToolsFilterPopup::changeEvent(QEvent* event)
{
	PopupWindow::changeEvent(event);

	if (event->type() == QEvent::LanguageChange) {
		retranslateStaticText();
		// 目录行里也有可翻译文案，整排重建最省事；展开/勾选状态都在模型里，重建不丢
		rebuild();
		updateStatus();
	}
}

TopBar::TopBar(QWidget* parent)
	: QWidget(parent)
	, m_filter(new ToolsFilter(this))
{
	setObjectName(QStringLiteral("topBar"));
	setAttribute(Qt::WA_StyledBackground, true);
	setFixedHeight(48);
	// 宽度与消息内容一致（见 Main.cpp 的宽度约定：1152 - 左右各 16px 留白）
	setFixedWidth(1120);

	m_layout = new QHBoxLayout(this);
	m_layout->setContentsMargins(16, 0, 16, 0);
	m_layout->setSpacing(8);

	m_titleLabel = new QLabel(this);
	m_titleLabel->setObjectName(QStringLiteral("topBarTitle"));
	m_layout->addWidget(m_titleLabel);
	m_layout->addStretch();

	// 工具栏右侧的工具过滤入口：图标由 ToolsFilterButton 自绘（见该类注释），因此这里不设
	// icon/iconSize —— objectName 与 QSS 规则也由它自己在构造里设好。
	m_toolsButton = new ToolsFilterButton(this);
	m_layout->addWidget(m_toolsButton);

	dshRegister("TopBar.004", m_toolsButton, qOverload<bool>(&QPushButton::clicked), this, &TopBar::openToolsFilter);

	setTitle(QString());
	retranslateUi();

	// 登记到全局注册表：插件可按 index 取到顶栏（它是 VirtualTopBar 接口的实现）。
	// 放在构造末尾 —— 登记出去的对象必须已经能用；覆盖语义见 CommonRegistry.h。
	CommonRegistry::instance().AddToRegistry(DshHostIndex::kTopBar, this);
}

TopBar::~TopBar()
{
	CommonRegistry::instance().Destroy(DshHostIndex::kTopBar, this);
}

void TopBar::setTitle(const QString& title)
{
	m_title = title;
	retranslateUi();
}

void TopBar::setSessionId(const QString& sessionId)
{
	if (sessionId == m_sessionId)
		return;

	m_sessionId = sessionId;

	// 会话换掉时把窗口收起来（它展示的是上一个会话的工具表）
	if (m_toolsPopup && m_toolsPopup->isVisible())
		closeToolsFilter();

	// 打开会话时异步拉一次目录；该会话还没有过滤配置就顺手建一份初始版
	loadTools(false);
}

void TopBar::setBaseUrlProvider(std::function<QUrl()> provider)
{
	m_baseUrlProvider = std::move(provider);
}

void TopBar::loadTools(bool pushToPopup)
{
	if (m_baseUrlProvider)
		m_filter->setBaseUrl(m_baseUrlProvider());

	if (m_sessionId.isEmpty()) {
		if (pushToPopup && m_toolsPopup)
			m_toolsPopup->setStatus(qtTrId("toolfilter_no_session"));
		return;
	}
	if (m_loading)
		return;

	m_loading = true;
	if (pushToPopup && m_toolsPopup)
		m_toolsPopup->setBusy(true);

	const QString requested = m_sessionId;
	m_filter->ensureSession(requested,
		[this, requested, pushToPopup](bool ok, const ToolFilterCatalog& catalog, bool created, const QString& error) {
			m_loading = false;

			// 期间又切了会话：这次结果作废
			if (requested != m_sessionId)
				return;

			if (!ok) {
				qWarning().noquote() << "[ToolsFilter] 读取工具目录失败 session=" << requested
					<< "error=" << error;
				if (pushToPopup && m_toolsPopup && m_toolsPopup->isVisible())
					m_toolsPopup->applyCatalog(catalog);
				return;
			}

			if (created)
				qInfo().noquote() << "[ToolsFilter] 已为该会话建立初始过滤配置 session=" << requested
				<< "tools=" << ToolsFilter::toolCount(catalog.directories);

			// 窗口开着（或正要打开）时把目录推给它
			if (m_toolsPopup && (pushToPopup || m_toolsPopup->isVisible())) {
				m_toolsPopup->setContext(m_filter, requested, catalog.dropGuidance, catalog.hideContexts);
				m_toolsPopup->applyCatalog(catalog);
			}
		});
}

// 工具过滤入口的开关。关掉时顺手收起已打开的窗口：它拉的是 DSH 专属的 /api/tools-filter，
// 接管后那个服务端已经不在了，留着只会显示失败。
void TopBar::setToolsFilterEnabled(bool enabled)
{
	// ⚠️ 别拿 isVisible() 当"当前是否已隐藏"判重：窗口还没 show() 时子控件一律 false（切主题重建
	// 窗口 + 插件重新接管正好走这条时序），会漏掉这次 setVisible(false)。直接设，重复调用无副作用。
	if (m_toolsButton)
		m_toolsButton->setVisible(enabled);

	if (!enabled)
		closeToolsFilter();
}

void TopBar::openToolsFilter()
{
	QWidget* host = window();
	if (!host)
		return;

	// 判重：已经打开就忽略重复点击
	if (m_toolsPopup && m_toolsPopup->isVisible())
		return;

	// 惰性建弹窗要排在铺遮罩之前：构造要花时间，而遮罩那次同步重绘必须紧贴弹窗 show()
	//（见 WindowFrame::showOverlayWithPopup）。
	if (!m_toolsPopup) {
		m_toolsPopup = new ToolsFilterPopup(host);
		dshRegister("TopBar.005", m_toolsPopup, &PopupWindow::closed, this, &TopBar::closeToolsFilter);
		dshRegister("TopBar.006", m_toolsPopup,
			&ToolsFilterPopup::refreshRequested, this, [this]() { loadTools(true); });
	}

	// 铺遮罩 + 居中 + 显示：背靠背完成，两者落在同一帧
	WindowFrame::showOverlayWithPopup(host, this, m_toolsPopup);

	// 数据随后异步加载。不预置上下文：保留上次 applyCatalog 注入的 DropGuidance / HideContexts，
	// loadTools(true) 随后会用服务端权威值覆盖它 —— 预置默认值会让"加载还没回来就点了勾选"写坏配置。
	loadTools(true);
}

void TopBar::closeToolsFilter()
{
	// 先收遮罩、再隐藏弹窗：收遮罩那步会同步重绘主窗口，两件事落在同一帧。反过来（或让
	// 遮罩等下一帧重绘）观感就是"弹窗没了、遮罩还留一拍"。
	if (QWidget* host = window())
		WindowFrame::hideOverlay(host, this);
	if (m_toolsPopup)
		m_toolsPopup->hide();
}

void TopBar::syncOverlayToHost()
{
	QWidget* host = window();
	if (!host)
		return;

	WindowFrame::syncOverlay(host);
	if (m_toolsPopup && m_toolsPopup->isVisible())
		m_toolsPopup->move(host->geometry().center() - m_toolsPopup->rect().center());
}

void TopBar::retranslateUi()
{
	if (m_titleLabel)
		m_titleLabel->setText(m_title.isEmpty() ? qtTrId("session_untitled") : m_title);
	if (m_toolsButton)
		m_toolsButton->setToolTip(qtTrId("toolfilter_title"));
}

void TopBar::changeEvent(QEvent* event)
{
	QWidget::changeEvent(event);

	if (event->type() == QEvent::LanguageChange)
		retranslateUi();
}
