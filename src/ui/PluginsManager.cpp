#include "PluginsManager.h"

#include "ThemeManager.h"
#include "WindowFrame.h"
#include "PluginMarketClient.h"
#include "PluginMarketInstaller.h"

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
	void clearLayout(QLayout* layout)
	{
		while (QLayoutItem* item = layout->takeAt(0)) {
			if (QWidget* widget = item->widget())
				widget->deleteLater();
			delete item;
		}
	}
}

PluginsManager::PluginsManager(const QUrl& baseUrl, QWidget* host)
	: StatusPopupWindow(host)
	, m_host(host)
	, m_market(new PluginMarketClient(this))
	, m_installer(new PluginMarketInstaller(this))
{
	m_market->setBaseUrl(baseUrl);
	setTitle(tr("插件市场"));

	auto* content = new QWidget(this);
	auto* rootLayout = new QVBoxLayout(content);
	rootLayout->setContentsMargins(0, 0, 0, 0);
	rootLayout->setSpacing(10);

	// 顶部工具栏
	auto* toolbar = new QHBoxLayout;
	toolbar->setSpacing(8);

	m_searchEdit = new QLineEdit(content);
	m_searchEdit->setPlaceholderText(tr("搜索插件..."));
	m_searchEdit->setClearButtonEnabled(true);
	m_searchEdit->setObjectName(QStringLiteral("pluginSearchEdit"));

	auto* refreshButton = new QPushButton(tr("刷新"), content);
	refreshButton->setObjectName(QStringLiteral("pluginRefreshButton"));
	m_restartButton = new QPushButton(tr("重启服务"), content);
	m_restartButton->setObjectName(QStringLiteral("pluginRestartButton"));

	for (QPushButton* button : { refreshButton, m_restartButton }) {
		button->setCursor(Qt::PointingHandCursor);
	}

	toolbar->addWidget(m_searchEdit, 1);
	toolbar->addWidget(refreshButton);
	toolbar->addWidget(m_restartButton);

	rootLayout->addLayout(toolbar);

	m_tabs = new QTabWidget(content);
	m_tabs->setObjectName(QStringLiteral("pluginTabs"));
	rootLayout->addWidget(m_tabs, 1);

	// ========== 插件市场页 ==========
	auto* marketPage = new QWidget(content);
	auto* marketLayout = new QVBoxLayout(marketPage);
	marketLayout->setContentsMargins(8, 8, 8, 8);
	marketLayout->setSpacing(8);

	m_marketScroll = new QScrollArea(marketPage);
	m_marketScroll->setObjectName(QStringLiteral("pluginMarketScroll"));
	m_marketScroll->setWidgetResizable(true);
	m_marketScroll->setFrameShape(QFrame::NoFrame);
	// 滚动条早于 objectName 存在（基类构造时创建），设完名字要重新解析一次，
	// 否则 #pluginMarketScroll QScrollBar 匹配不上、滚动条按原生样式画
	Theme::repolishScrollArea(m_marketScroll);

	m_marketContainer = new QWidget;
	m_marketContainer->setObjectName(QStringLiteral("pluginMarketContainer"));
	m_marketCardsLayout = new QVBoxLayout(m_marketContainer);
	m_marketCardsLayout->setContentsMargins(0, 0, 0, 0);
	m_marketCardsLayout->setSpacing(8);
	m_marketScroll->setWidget(m_marketContainer);

	marketLayout->addWidget(m_marketScroll, 1);

	// 分页栏
	auto* pageBar = new QHBoxLayout;
	m_prevButton = new QPushButton(tr("上一页"), marketPage);
	m_prevButton->setObjectName(QStringLiteral("pluginPrevPageButton"));
	m_nextButton = new QPushButton(tr("下一页"), marketPage);
	m_nextButton->setObjectName(QStringLiteral("pluginNextPageButton"));
	m_pageLabel = new QLabel(tr("第 1 / 1 页"), marketPage);
	m_pageLabel->setObjectName(QStringLiteral("pluginPageLabel"));
	m_pageLabel->setAlignment(Qt::AlignCenter);

	for (QPushButton* button : { m_prevButton, m_nextButton }) {
		button->setCursor(Qt::PointingHandCursor);
	}

	pageBar->addWidget(m_prevButton);
	pageBar->addStretch(1);
	pageBar->addWidget(m_pageLabel);
	pageBar->addStretch(1);
	pageBar->addWidget(m_nextButton);

	marketLayout->addLayout(pageBar);

	m_tabs->addTab(marketPage, tr("插件市场"));

	// ========== 已安装页 ==========
	auto* installedPage = new QWidget(content);
	auto* installedLayout = new QVBoxLayout(installedPage);
	installedLayout->setContentsMargins(8, 8, 8, 8);
	installedLayout->setSpacing(8);

	m_installedScroll = new QScrollArea(installedPage);
	m_installedScroll->setObjectName(QStringLiteral("pluginInstalledScroll"));
	m_installedScroll->setWidgetResizable(true);
	m_installedScroll->setFrameShape(QFrame::NoFrame);
	// 同上：#pluginInstalledScroll 的滚动条规则需要重新解析一次
	Theme::repolishScrollArea(m_installedScroll);

	m_installedContainer = new QWidget;
	m_installedContainer->setObjectName(QStringLiteral("pluginInstalledContainer"));
	m_installedCardsLayout = new QVBoxLayout(m_installedContainer);
	m_installedCardsLayout->setContentsMargins(0, 0, 0, 0);
	m_installedCardsLayout->setSpacing(8);
	m_installedScroll->setWidget(m_installedContainer);

	installedLayout->addWidget(m_installedScroll, 1);

	m_tabs->addTab(installedPage, tr("已安装"));

	// 安装进度
	m_progressBar = new QProgressBar(content);
	m_progressBar->setObjectName(QStringLiteral("pluginProgressBar"));
	m_progressBar->setRange(0, 0);
	m_progressBar->setTextVisible(false);
	m_progressBar->setFixedHeight(8);
	m_progressBar->hide();
	rootLayout->addWidget(m_progressBar);

	// 如果当前加载到的是快照，定时重新拉取，等后台刷新完成后自动切换到最新数据
	m_registryRefreshTimer = new QTimer(this);
	m_registryRefreshTimer->setInterval(3000);
	connect(m_registryRefreshTimer, &QTimer::timeout, this, [this]() {
		if (m_registryLoaded)
			refresh();
		});

	// 状态栏（初始文案走 setStatus，保证按宽度省略号截断）
	m_statusLabel = new QLabel(content);
	m_statusLabel->setObjectName(QStringLiteral("pluginStatusLabel"));
	attachStatusLabel(m_statusLabel); // 宽度布局生效后重排状态文案
	setStatus(tr("正在加载插件市场..."));
	rootLayout->addWidget(m_statusLabel);

	setContent(content);

	resize(820, 600);

	connect(refreshButton, &QPushButton::clicked, this, &PluginsManager::refresh);
	connect(m_restartButton, &QPushButton::clicked, this, [this]() {
		m_market->restartServer();
		});
	connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() {
		m_currentPage = 0;
		populateMarket();
		});
	connect(m_prevButton, &QPushButton::clicked, this, [this]() { changePage(-1); });
	connect(m_nextButton, &QPushButton::clicked, this, [this]() { changePage(1); });

	// 市场数据 / 操作结果全部来自 common 层客户端
	connect(m_market, &PluginMarketClient::registryLoaded, this, [this](const QJsonArray& plugins, const QString& source) {
		m_plugins = PluginMarketModel::parsePlugins(plugins);
		m_registryLoaded = true;
		populateMarket();

		if (source == QStringLiteral("snapshot")) {
			// 快照不是最新数据，轮询直到后台刷新完成，让市场自动切换到最新列表
			if (m_registryRefreshTimer && !m_registryRefreshTimer->isActive())
				m_registryRefreshTimer->start();
			setStatus(tr("插件市场已加载（离线快照），正在等待最新数据..."));
		}
		else {
			if (m_registryRefreshTimer)
				m_registryRefreshTimer->stop();
			if (source == QStringLiteral("cache")) {
				setStatus(tr("插件市场已加载（缓存），共 %1 个插件").arg(m_plugins.size()));
			}
			else {
				setStatus(tr("插件市场已加载，共 %1 个插件").arg(m_plugins.size()));
			}
		}
		});
	connect(m_market, &PluginMarketClient::registryFailed, this, [this](const QString& error, int) {
		setStatus(tr("加载插件市场失败: %1").arg(error));
		});
	connect(m_market, &PluginMarketClient::installedLoaded, this, &PluginsManager::onInstalledLoaded);
	connect(m_market, &PluginMarketClient::operationCompleted, this, &PluginsManager::onMarketOperationCompleted);
	connect(m_market, &PluginMarketClient::operationFailed, this, &PluginsManager::onMarketOperationFailed);

	// 市场包自动安装的进度与结果
	connect(m_installer, &PluginMarketInstaller::installStarted, this, [this]() {
		m_progressBar->show();
		setStatus(tr("未检测到插件市场，正在自动安装..."));
		});
	connect(m_installer, &PluginMarketInstaller::installOutput, this, [this](const QString& line) {
		setStatus(tr("正在安装插件市场: %1").arg(line.left(60)));
		});
	connect(m_installer, &PluginMarketInstaller::installFinished, this, [this](bool ok) {
		m_progressBar->hide();
		if (!ok) {
			setStatus(tr("插件市场安装失败，请检查网络或稍后重试"));
			return;
		}
		setStatus(tr("插件市场安装完成，正在重启服务..."));
		emit serverRestartRequested();
		retryRefreshAfterInstall(0);
		});

	// 常驻插件系统：窗口关闭（右上角 ✕）后自动清理遮罩并隐藏。
	// 对象本身不销毁，供下次 openPlugins() 复用。
	connect(this, &PopupWindow::closed, this, &PluginsManager::closePlugins);

	// 联网拉取不放在构造里：常驻对象在服务端可能尚未就绪，
	// 市场/已安装数据改由 openPlugins() 每次打开时经 refreshOnOpen() 拉取。
hide();
}

// ------------------------------------------------------------------
// 窗口开关管理（插件系统自管，不再由 DSHHub 代管）
// ------------------------------------------------------------------

void PluginsManager::openPlugins()
{
	if (!m_host)
		return;
	// 判重：窗口已经打开时忽略重复请求
	if (isVisible())
		return;

	// 遮罩：宿主主窗口的子控件，铺满内容区并盖住主界面
	// （不含自绘标题栏，否则窗口按钮会被一起盖住点不动）
	if (!m_overlay) {
		m_overlay = new QWidget(m_host);
		m_overlay->setObjectName(QStringLiteral("pluginsOverlay"));
		m_overlay->setAttribute(Qt::WA_StyledBackground, true);
	}
	m_overlay->setGeometry(WindowFrame::overlayRect(m_host));
	m_overlay->raise();
	m_overlay->show();
	// 强制先让遮罩画出来（否则与下方弹窗同一帧才呈现，观感像弹窗先出、遮罩延迟）
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

	// 打开前刷新数据（市场 + 已安装 + 确保市场包存在）
	refreshOnOpen();

	// 居中于宿主并显示
	move(m_host->geometry().center() - rect().center());
	show();
	raise();
}

void PluginsManager::closePlugins()
{
	// 关闭遮罩（常驻复用：只隐藏，不销毁，避免每次开关重新创建全窗半透明控件）
	if (m_overlay)
		m_overlay->hide();
	// 隐藏自己（常驻：不销毁，等待下次打开）
hide();
}

void PluginsManager::syncOverlayToHost()
{
	if (m_overlay && m_host)
		m_overlay->setGeometry(WindowFrame::overlayRect(m_host));
}

void PluginsManager::refreshOnOpen()
{
	// 市场与已安装列表（异步，服务端未就绪时状态栏会给出失败提示）
	refresh();
	// 确保插件市场包已安装（已存在则本方法立即返回）
	m_installer->ensureInstalled(QCoreApplication::applicationDirPath());
}

void PluginsManager::setBaseUrl(const QUrl& url)
{
	m_market->setBaseUrl(url);
}

void PluginsManager::retryRefreshAfterInstall(int attempt)
{
	if (m_registryLoaded)
		return;

	refresh();
	if (attempt < 6) {
		const int delay = attempt == 0 ? 500 : 1000;
		QTimer::singleShot(delay, this, [this, attempt]() {
			retryRefreshAfterInstall(attempt + 1);
			});
	}
}

void PluginsManager::refresh()
{
	m_registryLoaded = false;
	setStatus(tr("正在加载插件市场..."));
	m_market->fetchRegistry();
	m_market->fetchInstalled();
}

// ------------------------------------------------------------------
// 客户端回调
// ------------------------------------------------------------------

void PluginsManager::onInstalledLoaded(const QJsonObject& installed)
{
	m_installed = installed;
	populateInstalled();
}

void PluginsManager::onMarketOperationCompleted(const QString& path)
{
	Q_UNUSED(path);

	setStatus(tr("操作已提交，正在刷新..."));
	refresh();
}

void PluginsManager::onMarketOperationFailed(const QString& path, const QString& error, int status)
{
	// 路径与状态码已经由 PluginMarketClient 打进日志，这里只需要展示给用户
	Q_UNUSED(path);
	Q_UNUSED(status);

	setStatus(tr("操作失败: %1").arg(error));
}

// ------------------------------------------------------------------
// 列表绘制
// ------------------------------------------------------------------

void PluginsManager::populateMarket()
{
	m_filteredPlugins = PluginMarketModel::filter(m_plugins, m_searchEdit->text());

	m_currentPage = 0;
	renderMarketPage();
}

void PluginsManager::renderMarketPage()
{
	clearLayout(m_marketCardsLayout);

	const PluginMarketModel::Page page = PluginMarketModel::paginate(
		m_filteredPlugins.size(), m_pageSize, m_currentPage);
	m_currentPage = page.index;

	for (int i = page.begin; i < page.end; ++i)
		m_marketCardsLayout->addWidget(createMarketCard(m_filteredPlugins.at(i)));

	m_marketCardsLayout->addStretch(1);

	m_pageLabel->setText(tr("第 %1 / %2 页").arg(m_currentPage + 1).arg(page.count));
	m_prevButton->setEnabled(m_currentPage > 0);
	m_nextButton->setEnabled(m_currentPage + 1 < page.count);
}

void PluginsManager::populateInstalled()
{
	clearLayout(m_installedCardsLayout);
	for (auto it = m_installed.begin(); it != m_installed.end(); ++it) {
		m_installedCardsLayout->addWidget(createInstalledCard(it.key(), it.value().toString()));
	}
	m_installedCardsLayout->addStretch(1);
}

void PluginsManager::changePage(int delta)
{
	const PluginMarketModel::Page page = PluginMarketModel::paginate(
		m_filteredPlugins.size(), m_pageSize, m_currentPage + delta);
	m_currentPage = page.index;
	renderMarketPage();
}

QWidget* PluginsManager::createMarketCard(const MarketPlugin& plugin)
{
	auto* card = new QFrame;
	card->setObjectName(QStringLiteral("pluginCard"));

	auto* layout = new QHBoxLayout(card);
	layout->setContentsMargins(14, 12, 14, 12);
	layout->setSpacing(12);

	auto* infoLayout = new QVBoxLayout;
	infoLayout->setSpacing(4);

	auto* titleRow = new QHBoxLayout;
	titleRow->setSpacing(8);

	auto* nameLabel = new QLabel(plugin.name, card);
	nameLabel->setObjectName(QStringLiteral("pluginCardNameLabel"));

	auto* categoryLabel = new QLabel(plugin.category, card);
	categoryLabel->setObjectName(QStringLiteral("pluginCardCategoryLabel"));

	titleRow->addWidget(nameLabel);
	if (!plugin.category.isEmpty())
		titleRow->addWidget(categoryLabel);
	titleRow->addStretch(1);

	infoLayout->addLayout(titleRow);

	const QString displayDesc = plugin.displayDescription();
	if (!displayDesc.isEmpty()) {
		auto* descLabel = new QLabel(displayDesc, card);
		descLabel->setWordWrap(true);
		descLabel->setObjectName(QStringLiteral("pluginCardDescLabel"));
		infoLayout->addWidget(descLabel);
	}

	layout->addLayout(infoLayout, 1);

	auto* installButton = new QPushButton(tr("安装"), card);
	installButton->setObjectName(QStringLiteral("pluginCardInstallButton"));
	installButton->setCursor(Qt::PointingHandCursor);
	const QString url = plugin.url;
	connect(installButton, &QPushButton::clicked, this, [this, url]() {
		m_market->installPlugin(url);
		});

	layout->addWidget(installButton, 0, Qt::AlignVCenter);

	return card;
}

QWidget* PluginsManager::createInstalledCard(const QString& name, const QString& version)
{
	auto* card = new QFrame;
	card->setObjectName(QStringLiteral("pluginCard"));

	auto* layout = new QHBoxLayout(card);
	layout->setContentsMargins(14, 12, 14, 12);
	layout->setSpacing(12);

	auto* nameLabel = new QLabel(
		version.isEmpty() ? name : QStringLiteral("%1  %2").arg(name, version),
		card);
	nameLabel->setObjectName(QStringLiteral("pluginCardNameLabel"));

	layout->addWidget(nameLabel, 1);

	auto* updateButton = new QPushButton(tr("更新"), card);
	updateButton->setObjectName(QStringLiteral("pluginCardUpdateButton"));
	auto* uninstallButton = new QPushButton(tr("卸载"), card);
	uninstallButton->setObjectName(QStringLiteral("pluginCardUninstallButton"));

	for (QPushButton* button : { updateButton, uninstallButton }) {
		button->setCursor(Qt::PointingHandCursor);
	}

	connect(updateButton, &QPushButton::clicked, this, [this, name]() {
		m_market->updatePlugin(name);
		});
	connect(uninstallButton, &QPushButton::clicked, this, [this, name]() {
		m_market->uninstallPlugin(name);
		});

	layout->addWidget(updateButton);
	layout->addWidget(uninstallButton);

	return card;
}