#include "TopBar.h"

#include "ThemeManager.h"
#include "WindowFrame.h"

#include <QDebug>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
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
