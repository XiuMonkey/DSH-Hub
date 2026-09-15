#include "TopBar.h"

#include "ThemeManager.h"
#include "WindowFrame.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QUrl>
#include <QVBoxLayout>

#include <utility>

namespace
{
	// 顶栏按钮图标：三根递减的横线（"过滤"），颜色取自当前主题 ——
	// 主题切换会重建主窗口，所以这里不必做动态换色。
	QIcon makeFilterIcon()
	{
		constexpr int kSize = 20;
		QPixmap pixmap(kSize, kSize);
		pixmap.fill(Qt::transparent);

		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing, true);
		// 注意括号形式：QPen pen(QColor(Theme::textSecondary())) 会被解析成函数声明
		const QColor iconColor(Theme::textSecondary());
		QPen pen(iconColor);
		pen.setWidthF(1.8);
		pen.setCapStyle(Qt::RoundCap);
		painter.setPen(pen);
		painter.drawLine(QPointF(3.0, 6.0), QPointF(17.0, 6.0));
		painter.drawLine(QPointF(5.5, 10.0), QPointF(14.5, 10.0));
		painter.drawLine(QPointF(8.0, 14.0), QPointF(12.0, 14.0));
		painter.end();

		return QIcon(pixmap);
	}

	// 列表项里存的角色：行号（勾选变化时按下标回写模型）
	constexpr int kRowIndexRole = Qt::UserRole + 1;
} // namespace

// ------------------------------------------------------------------
// ToolsFilterPopup
// ------------------------------------------------------------------

ToolsFilterPopup::ToolsFilterPopup(QWidget* parent)
	: StatusPopupWindow(parent)
{
	setObjectName(QStringLiteral("toolsFilterPopup"));
	setTitle(tr("工具过滤"));
	setFixedSize(460, 560);

	auto* content = new QWidget(this);
	auto* layout = new QVBoxLayout(content);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(10);

	m_hint = new QLabel(content);
	m_hint->setObjectName(QStringLiteral("toolsFilterHint"));
	m_hint->setWordWrap(true);
	layout->addWidget(m_hint);

	m_list = new QListWidget(content);
	m_list->setObjectName(QStringLiteral("toolsFilterList"));
	m_list->setSelectionMode(QAbstractItemView::NoSelection);
	m_list->setUniformItemSizes(false);
	// 滚动条是 QListWidget 基类构造时建好的，那时 objectName 还没设 —— 见头文件里
	// Theme::repolishScrollArea 的说明（下面这句顺带把"首次显示/首次真出现"也补上）
	Theme::repolishScrollArea(m_list);
	layout->addWidget(m_list, 1);

	auto* buttonRow = new QHBoxLayout;
	buttonRow->setContentsMargins(0, 0, 0, 0);
	buttonRow->setSpacing(8);

	m_showAllButton = new QPushButton(tr("全部显示"), content);
	m_showAllButton->setObjectName(QStringLiteral("toolsFilterShowAllButton"));
	m_showAllButton->setCursor(Qt::PointingHandCursor);

	m_hideAllButton = new QPushButton(tr("全部隐藏"), content);
	m_hideAllButton->setObjectName(QStringLiteral("toolsFilterHideAllButton"));
	m_hideAllButton->setCursor(Qt::PointingHandCursor);

	m_refreshButton = new QPushButton(tr("刷新"), content);
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

	connect(m_list, &QListWidget::itemChanged, this, &ToolsFilterPopup::onItemChanged);
	connect(m_showAllButton, &QPushButton::clicked, this, [this]() { setAllVisible(true); });
	connect(m_hideAllButton, &QPushButton::clicked, this, [this]() { setAllVisible(false); });
	connect(m_refreshButton, &QPushButton::clicked, this, [this]() { emit refreshRequested(); });

	retranslateStaticText();
	updateStatus();
}

void ToolsFilterPopup::retranslateStaticText()
{
	if (m_hint) {
		m_hint->setText(tr("取消勾选 = 该工具不再出现在发给模型的清单里（省 token），但它仍然可以被调用。"
			"没有工具描述与参数写进配置文件，那些只在这里做提示。"));
	}
	if (m_showAllButton)
		m_showAllButton->setText(tr("全部显示"));
	if (m_hideAllButton)
		m_hideAllButton->setText(tr("全部隐藏"));
	if (m_refreshButton)
		m_refreshButton->setText(tr("刷新"));
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
		setStatus(tr("正在读取该会话的工具目录…"));
}

void ToolsFilterPopup::applyCatalog(const ToolFilterCatalog& catalog)
{
	if (!catalog.ok) {
		m_tools.clear();
		populate();
		m_degraded = false;
		setStatus(catalog.error.isEmpty() ? tr("读取工具目录失败") : catalog.error);
		return;
	}

	m_sessionId = catalog.sessionId;
	m_dropGuidance = catalog.dropGuidance;
	m_hideContexts = catalog.hideContexts;
	m_degraded = catalog.degraded;
	m_tools = catalog.tools;
	populate();
	updateStatus();
}

void ToolsFilterPopup::populate()
{
	m_updating = true;
	m_list->clear();

	for (int row = 0; row < m_tools.size(); ++row) {
		const ToolFilterEntry& tool = m_tools.at(row);
		auto* item = new QListWidgetItem(tool.name, m_list);
		item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
		item->setCheckState(tool.visible ? Qt::Checked : Qt::Unchecked);
		item->setData(kRowIndexRole, row);

		// 描述与参数只在这里出现（不写进配置文件）
		QString tip = tool.description;
		if (!tool.parameters.isEmpty() && tool.parameters != QStringLiteral("{}"))
			tip += QStringLiteral("\n\n%1 %2").arg(tr("参数："), tool.parameters);
		if (!tip.isEmpty())
			item->setToolTip(tip);
	}

	m_updating = false;
}

QVector<ToolFilterEntry> ToolsFilterPopup::collectEntries() const
{
	return m_tools;
}

void ToolsFilterPopup::onItemChanged(QListWidgetItem* item)
{
	if (m_updating || !item)
		return;

	const int row = item->data(kRowIndexRole).toInt();
	if (row < 0 || row >= m_tools.size())
		return;

	m_tools[row].visible = item->checkState() == Qt::Checked;
	saveNow();
	updateStatus();
}

void ToolsFilterPopup::setAllVisible(bool visible)
{
	if (m_tools.isEmpty())
		return;

	for (ToolFilterEntry& tool : m_tools)
		tool.visible = visible;

	populate();
	saveNow();
	updateStatus();
}

void ToolsFilterPopup::saveNow()
{
	if (!m_filter || m_sessionId.isEmpty()) {
		setStatus(tr("还没有打开的会话"));
		return;
	}

	setStatus(tr("正在保存…"));
	const QString sessionId = m_sessionId;
	const QVector<ToolFilterEntry> tools = collectEntries();
	const bool dropGuidance = m_dropGuidance;
	const QStringList hideContexts = m_hideContexts;

	// 异步写回：回调里再更新状态，绝不阻塞界面
	m_filter->save(sessionId, tools, dropGuidance, hideContexts,
		[this, sessionId](bool ok, const QString& error) {
			if (sessionId != m_sessionId)
				return; // 会话已经切走，这次结果作废
			if (!ok) {
				setStatus(error.isEmpty() ? tr("保存失败") : error);
				return;
			}
			updateStatus();
		});
}

void ToolsFilterPopup::updateStatus()
{
	if (m_tools.isEmpty()) {
		setStatus(tr("该会话没有可过滤的工具"));
		return;
	}

	QString text = tr("共 %1 个工具，已隐藏 %2 个（隐藏只影响提示词）")
		.arg(m_tools.size())
		.arg(ToolsFilter::hiddenCount(m_tools));
	if (m_degraded)
		text += tr(" · 服务端只答出了全局层，目录可能不完整");
	setStatus(text);
}

void ToolsFilterPopup::changeEvent(QEvent* event)
{
	PopupWindow::changeEvent(event);

	if (event->type() == QEvent::LanguageChange) {
		retranslateStaticText();
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

	// 工具栏右侧：工具过滤入口
	m_toolsButton = new QPushButton(this);
	m_toolsButton->setObjectName(QStringLiteral("topBarToolsButton"));
	m_toolsButton->setFixedSize(32, 32);
	m_toolsButton->setCursor(Qt::PointingHandCursor);
	m_toolsButton->setIcon(makeFilterIcon());
	m_toolsButton->setIconSize(QSize(20, 20));
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
			m_toolsPopup->setStatus(tr("还没有打开的会话"));
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
					<< "tools=" << catalog.tools.size();

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

	// 遮罩：宿主主窗口的子控件，铺满内容区并盖住主界面
	// （不含自绘标题栏，否则窗口按钮会被一起盖住点不动）
	if (!m_toolsOverlay) {
		m_toolsOverlay = new QWidget(host);
		m_toolsOverlay->setObjectName(QStringLiteral("toolsFilterOverlay"));
		m_toolsOverlay->setAttribute(Qt::WA_StyledBackground, true);
	}
	m_toolsOverlay->setGeometry(WindowFrame::overlayRect(host));
	m_toolsOverlay->raise();
	m_toolsOverlay->show();
	// 先让遮罩画出来（否则与弹窗同一帧才呈现，观感像弹窗先出、遮罩延迟）
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

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
	if (m_toolsOverlay)
		m_toolsOverlay->hide();
	if (m_toolsPopup)
		m_toolsPopup->hide();
}

void TopBar::syncOverlayToHost()
{
	QWidget* host = window();
	if (!host)
		return;

	if (m_toolsOverlay && m_toolsOverlay->isVisible())
		m_toolsOverlay->setGeometry(WindowFrame::overlayRect(host));
	if (m_toolsPopup && m_toolsPopup->isVisible())
		m_toolsPopup->move(host->geometry().center() - m_toolsPopup->rect().center());
}

void TopBar::retranslateUi()
{
	if (m_titleLabel)
		m_titleLabel->setText(m_title.isEmpty() ? tr("未命名会话") : m_title);
	if (m_toolsButton)
		m_toolsButton->setToolTip(tr("工具过滤"));
}

void TopBar::changeEvent(QEvent* event)
{
	QWidget::changeEvent(event);

	if (event->type() == QEvent::LanguageChange)
		retranslateUi();
}
