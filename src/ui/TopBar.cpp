#include "TopBar.h"

#include "ThemeManager.h"
#include "WindowFrame.h"

#include <QCheckBox>
#include <QDebug>
#include <QEvent>
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
	// 顶栏右侧的"工具过滤"按钮：图标是三条递减的胶囊，直接在 paintEvent 里画。
	//
	// 为什么不用 QIcon + QPixmap 位图（原来的写法）：
	//   1) 位图得按 devicePixelRatio 预生成。只给 20×20、DPR=1 的位图，在 125%/150%
	//      缩放的显示器上会被 Qt 放大 → 整条线发虚（本项目的 Logo 是按 @2x 处理的，
	//      图标这里漏了）；
	//   2) 更要紧的是它在**不缩放**时也不锐：QPen 宽 1.8、圆心落在整数 y=6/10/14 上，
	//      描边覆盖 [5.1, 6.9]，横跨第 5、6 两行各 91% —— 实测整张图标**一个满覆盖
	//      像素都没有**（最大 alpha 232/255），所以看着发灰、不干净。
	// 直接矢量画、并把几何对齐到设备像素（实心胶囊、整高、整数坐标）就没有这两个问题：
	// 实测满覆盖像素 0 → 44，最大 alpha 232 → 255，线正好落在整行像素上。
	//
	// 颜色每次绘制现取 Theme::textSecondary()，不缓存 —— 主题是重建窗口切换的，
	// 这样连"忘了跟着换色"的机会都没有。
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

			const qreal dpr = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
			// 20×20 的设计栅格换算到设备像素后取整：每条线的上下边都落在像素边界上，
			// 不用靠抗锯齿"猜"，也就不会发灰。
			const auto dev = [dpr](qreal value) { return std::round(value * dpr); };

			QPainter painter(this);
			painter.setRenderHint(QPainter::Antialiasing, true); // 只有胶囊两端的圆头需要
			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(Theme::textSecondary()));
			// 之后 1 单位 = 1 设备像素
			painter.scale(1.0 / dpr, 1.0 / dpr);

			const qreal grid = dev(20.0);
			const qreal originX = std::round((width() * dpr - grid) / 2.0);
			const qreal originY = std::round((height() * dpr - grid) / 2.0);

			const auto bar = [&](qreal x1, qreal x2, qreal y) {
				const QRectF rect(originX + dev(x1), originY + dev(y),
					dev(x2) - dev(x1), dev(2.0));
				painter.drawRoundedRect(rect, rect.height() / 2.0, rect.height() / 2.0);
				};
			bar(3.0, 17.0, 6.0);   // 最宽：14
			bar(5.0, 15.0, 10.0);  // 中：10
			bar(8.0, 12.0, 14.0);  // 最窄：4（三根都以 x=10 居中）
		}
	};

	// 目录表头的箭头，字形与 ModelListEntry 的成员行同一套（U+25BE / U+25B8）。
	//
	// 这里刻意用 QChar(码点) 而不是别的写法：
	//   * QLatin1String("\u25BE")：窄字面量先按执行字符集编码（本项目 /utf-8）
	//     变成 E2 96 BE 三个字节，QLatin1String 再把这些字节逐字节当成 Latin-1，
	//     界面上就是三个乱码字形（本次要修的现场）。
	//   * 直接写字面量：依赖源文件一定被当成 UTF-8 读（本机是 CP936 时整条字符串
	//     还可能被截断）。
	// QChar(0x25BE) 源码全 ASCII，与源文件编码、执行字符集都无关。
	const QString kChevronExpanded(QChar(0x25BE));
	const QString kChevronCollapsed(QChar(0x25B8));

	// 目录表头的最小高度：两行文字（目录名 + 计数）+ 上下内边距。不显式给下限时
	// QPushButton 会按"一行文本 + 按钮内边距"算高度，第二行会被压掉
	// （ModelListEntry 踩过同一个坑）。
	constexpr int kDirectoryHeaderMinHeight = 44;
	constexpr int kDirectoryHeaderPadding = 6;
} // namespace

// ------------------------------------------------------------------
// ToolsFilterDirectoryEntry
// ------------------------------------------------------------------

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

	// ---------------- 工具行 ----------------
	m_body = new QWidget(this);
	m_body->setObjectName(QStringLiteral("toolsFilterDirectoryBody"));

	auto* bodyLayout = new QVBoxLayout(m_body);
	bodyLayout->setContentsMargins(10, 0, 10, 8);
	bodyLayout->setSpacing(2);

	if (directory.tools.isEmpty()) {
		// 目录底下什么都没有时也得说一句：否则展开后是一片空白，
		// 看起来像"点坏了"（Default 目录在配置认领了全部工具之外的工具时才空）
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

		connect(box, &QCheckBox::toggled, this, [this, index](bool checked) {
			refreshMeta();
			emit toolVisibilityChanged(m_name, m_toolNames.at(index), checked);
			});

		m_boxes.append(box);
		m_toolNames.append(tool.name);
		bodyLayout->addWidget(box);
	}

	layout->addWidget(m_body);

	refreshMeta();
	setExpanded(true);

	connect(m_header, &QPushButton::clicked, this, [this]() {
		setExpanded(!isExpanded());
		});
}

QString ToolsFilterDirectoryEntry::directoryName() const
{
	return m_name;
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
	// 配置里写了 IsExpanded:"False"（插件层面整组隐藏）时要说明白：
	// 那时候下面的勾选状态其实不起作用，不说的话界面在撒谎
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

// ------------------------------------------------------------------
// ToolsFilterPopup
// ------------------------------------------------------------------

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
	m_scroll = new QScrollArea(content);
	m_scroll->setObjectName(QStringLiteral("toolsFilterScroll"));
	m_scroll->setFrameShape(QFrame::NoFrame);
	m_scroll->setWidgetResizable(true);
	m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	// 滚动条在基类构造时就建好了，那时 objectName 还没设 —— 见 ThemeManager.h 里
	// Theme::repolishScrollArea 的说明（下面这句顺带把"首次显示/首次真出现"也补上）
	Theme::repolishScrollArea(m_scroll);
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

	connect(m_showAllButton, &QPushButton::clicked, this, [this]() { setAllVisible(true); });
	connect(m_hideAllButton, &QPushButton::clicked, this, [this]() { setAllVisible(false); });
	connect(m_refreshButton, &QPushButton::clicked, this, [this]() { emit refreshRequested(); });

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

	// 换会话：展开状态是"这一次翻看"的状态，不带去别的会话（默认全展开）
	const bool sameSession = catalog.sessionId == m_sessionId;
	if (!sameSession)
		m_collapsed.clear();

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

	// 只 deleteLater() 的话旧行在真正销毁前仍是子控件，会被一并显示出来，
	// 于是列表里出现重复目录 —— 这里立刻从控件树上摘下来（同 ModelListPanel）。
	while (QLayoutItem* item = m_listLayout->takeAt(0)) {
		if (QWidget* widget = item->widget()) {
			widget->hide();
			widget->setParent(nullptr);
			widget->deleteLater();
		}
		delete item;
	}

	for (const ToolFilterDirectory& directory : m_directories) {
		auto* entry = new ToolsFilterDirectoryEntry(directory, m_listContent);
		// 先摆好展开状态再接线：setExpanded() 会发信号，接线在前等于自己写回自己
		entry->setExpanded(!m_collapsed.contains(directory.name));

		connect(entry, &ToolsFilterDirectoryEntry::expandedChanged,
			this, &ToolsFilterPopup::onDirectoryExpandedChanged);
		connect(entry, &ToolsFilterDirectoryEntry::toolVisibilityChanged,
			this, &ToolsFilterPopup::onToolVisibilityChanged);

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
	// 只记"这一次翻看"的显示状态，不写回配置：配置里的 IsExpanded 是筛选语义
	// （"False" = 整组隐藏），与这里的展开/收起不是一回事。
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

	QString text = qtTrId("toolfilter_summary_full_fmt")
		.arg(count)
		.arg(m_directories.size())
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
		// 目录行里也有可翻译文案（计数、空目录提示、表头提示）：整排重建最省事，
		// 展开状态与勾选状态都在模型里，重建不丢
		rebuild();
		updateStatus();
	}
}

// ------------------------------------------------------------------
// TopBar
// ------------------------------------------------------------------

TopBar::TopBar(QWidget* parent)
	: QWidget(parent)
	, m_filter(new ToolsFilter(this))
{
	setObjectName(QStringLiteral("topBar"));
	setAttribute(Qt::WA_StyledBackground, true);
	setFixedHeight(48);
	// 宽度与消息内容一致（见 Main.cpp 的宽度约定：1152 - 左右各 16px 留白）
	setFixedWidth(1120);

	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(16, 0, 16, 0);
	layout->setSpacing(8);

	m_titleLabel = new QLabel(this);
	m_titleLabel->setObjectName(QStringLiteral("topBarTitle"));
	layout->addWidget(m_titleLabel);
	layout->addStretch();

	// 工具栏右侧：工具过滤入口。
	// 图标在 ToolsFilterButton::paintEvent 里矢量绘制（清晰度与 devicePixelRatio
	// 的处理见该类的注释），所以这里不设 icon/iconSize —— objectName 与 QSS 规则
	// 由该类自己在构造里设好。
	m_toolsButton = new ToolsFilterButton(this);
	layout->addWidget(m_toolsButton);

	connect(m_toolsButton, &QPushButton::clicked, this, &TopBar::openToolsFilter);

	setTitle(QString());
	retranslateUi();
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

void TopBar::openToolsFilter()
{
	QWidget* host = window();
	if (!host)
		return;

	// 判重：已经打开就忽略重复点击
	if (m_toolsPopup && m_toolsPopup->isVisible())
		return;

	// 遮罩：宿主主窗口上那唯一一层半透明控件（铺满内容区、不含自绘标题栏，
	// 否则窗口按钮会被一起盖住点不动）。showOverlay() 里带一次同步重绘，
	// 所以下面直接 show() 弹窗就行，不会再出现"弹窗先出、遮罩后到"。
	WindowFrame::showOverlay(host, this);

	if (!m_toolsPopup) {
		m_toolsPopup = new ToolsFilterPopup(host);
		connect(m_toolsPopup, &PopupWindow::closed, this, &TopBar::closeToolsFilter);
		connect(m_toolsPopup, &ToolsFilterPopup::refreshRequested, this, [this]() { loadTools(true); });
	}

	// 不在这里预置上下文：窗口保留上一次 applyCatalog 注入的 DropGuidance /
	// HideContexts，紧接着的 loadTools(true) 会用服务端的权威值覆盖它 ——
	// 预置一个默认值会让"加载还没回来就点了勾选"这种情况把配置写坏。
	m_toolsPopup->move(host->geometry().center() - m_toolsPopup->rect().center());
	m_toolsPopup->show();
	m_toolsPopup->raise();

	loadTools(true);
}

void TopBar::closeToolsFilter()
{
	// 先收遮罩、再隐藏弹窗：收遮罩那一步会同步重绘一次主窗口，两件事落在
	// 同一帧上。反过来（或让遮罩等下一帧重绘）观感就是"弹窗没了、遮罩还留一拍"。
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