#include "PluginsPopup.h"
#include "ThemeManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
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

PluginsPopup::PluginsPopup(const QUrl& baseUrl, QWidget* parent)
	: PopupWindow(parent)
	, m_baseUrl(baseUrl)
	, m_nam(new QNetworkAccessManager(this))
{
	setTitle(QStringLiteral("插件市场"));

	auto* content = new QWidget(this);
	auto* rootLayout = new QVBoxLayout(content);
	rootLayout->setContentsMargins(0, 0, 0, 0);
	rootLayout->setSpacing(10);

	// 顶部工具栏
	auto* toolbar = new QHBoxLayout;
	toolbar->setSpacing(8);

	m_searchEdit = new QLineEdit(content);
	m_searchEdit->setPlaceholderText(QStringLiteral("搜索插件..."));
	m_searchEdit->setClearButtonEnabled(true);
	m_searchEdit->setStyleSheet(QStringLiteral("QLineEdit {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("inputBg")) + QStringLiteral(";") + QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";") + QStringLiteral("  border-radius: 10px;") + QStringLiteral("  padding: 8px 12px;") + QStringLiteral("  font-size: 13px;") + QStringLiteral("}") + QStringLiteral("QLineEdit:focus {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border-color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";") + QStringLiteral("}"));

	auto* refreshButton = new QPushButton(QStringLiteral("刷新"), content);
	m_restartButton = new QPushButton(QStringLiteral("重启服务"), content);

	for (QPushButton* button : { refreshButton, m_restartButton }) {
		button->setCursor(Qt::PointingHandCursor);
		button->setStyleSheet(QStringLiteral("QPushButton {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg")) + QStringLiteral(";") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 10px;") + QStringLiteral("  padding: 8px 14px;") + QStringLiteral("  font-size: 13px;") + QStringLiteral("}") + QStringLiteral("QPushButton:hover {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";") + QStringLiteral("}"));
	}

	toolbar->addWidget(m_searchEdit, 1);
	toolbar->addWidget(refreshButton);
	toolbar->addWidget(m_restartButton);

	rootLayout->addLayout(toolbar);

	m_tabs = new QTabWidget(content);
	m_tabs->setStyleSheet(QStringLiteral("QTabWidget::pane {") + QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";") + QStringLiteral("  border-radius: 12px;") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QTabBar::tab {") + QStringLiteral("  background: transparent;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textSecondary")) + QStringLiteral(";") + QStringLiteral("  padding: 8px 18px;") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 10px;") + QStringLiteral("  font-size: 13px;") + QStringLiteral("}") + QStringLiteral("QTabBar::tab:selected {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("activeBg")) + QStringLiteral(";") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("  font-weight: 600;") + QStringLiteral("}"));
	rootLayout->addWidget(m_tabs, 1);

	// ========== 插件市场页 ==========
	auto* marketPage = new QWidget(content);
	auto* marketLayout = new QVBoxLayout(marketPage);
	marketLayout->setContentsMargins(8, 8, 8, 8);
	marketLayout->setSpacing(8);

	m_marketScroll = new QScrollArea(marketPage);
	m_marketScroll->setWidgetResizable(true);
	m_marketScroll->setFrameShape(QFrame::NoFrame);
	m_marketScroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }") + QStringLiteral("QScrollArea > QWidget > QWidget { background: transparent; }") + QStringLiteral("QScrollArea QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }") + QStringLiteral("QScrollArea QScrollBar::handle:vertical { background: ") + Theme::color(QStringLiteral("scrollbar")) + QStringLiteral("; border-radius: 4px; min-height: 30px; }") + QStringLiteral("QScrollArea QScrollBar::handle:vertical:hover { background: ") + Theme::color(QStringLiteral("scrollbarHover")) + QStringLiteral("; }") + QStringLiteral("QScrollArea QScrollBar::add-line:vertical, QScrollArea QScrollBar::sub-line:vertical { height: 0; }") + QStringLiteral("QScrollArea QScrollBar::add-page:vertical, QScrollArea QScrollBar::sub-page:vertical { background: transparent; }"));

	m_marketContainer = new QWidget;
	m_marketCardsLayout = new QVBoxLayout(m_marketContainer);
	m_marketCardsLayout->setContentsMargins(0, 0, 0, 0);
	m_marketCardsLayout->setSpacing(8);
	m_marketScroll->setWidget(m_marketContainer);

	marketLayout->addWidget(m_marketScroll, 1);

	// 分页栏
	auto* pageBar = new QHBoxLayout;
	m_prevButton = new QPushButton(QStringLiteral("上一页"), marketPage);
	m_nextButton = new QPushButton(QStringLiteral("下一页"), marketPage);
	m_pageLabel = new QLabel(QStringLiteral("第 1 / 1 页"), marketPage);
	m_pageLabel->setAlignment(Qt::AlignCenter);
	m_pageLabel->setStyleSheet(QStringLiteral("QLabel { color: ") + Theme::color(QStringLiteral("textSecondary")) + QStringLiteral("; font-size: 12px; background: transparent; }"));

	for (QPushButton* button : { m_prevButton, m_nextButton }) {
		button->setCursor(Qt::PointingHandCursor);
		button->setStyleSheet(QStringLiteral("QPushButton {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg")) + QStringLiteral(";") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 10px;") + QStringLiteral("  padding: 6px 14px;") + QStringLiteral("  font-size: 12px;") + QStringLiteral("}") + QStringLiteral("QPushButton:hover {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QPushButton:disabled {") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("scrollbarHover")) + QStringLiteral(";") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("inputBg")) + QStringLiteral(";") + QStringLiteral("}"));
	}

	pageBar->addWidget(m_prevButton);
	pageBar->addStretch(1);
	pageBar->addWidget(m_pageLabel);
	pageBar->addStretch(1);
	pageBar->addWidget(m_nextButton);

	marketLayout->addLayout(pageBar);

	m_tabs->addTab(marketPage, QStringLiteral("插件市场"));

	// ========== 已安装页 ==========
	auto* installedPage = new QWidget(content);
	auto* installedLayout = new QVBoxLayout(installedPage);
	installedLayout->setContentsMargins(8, 8, 8, 8);
	installedLayout->setSpacing(8);

	m_installedScroll = new QScrollArea(installedPage);
	m_installedScroll->setWidgetResizable(true);
	m_installedScroll->setFrameShape(QFrame::NoFrame);
	m_installedScroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }") + QStringLiteral("QScrollArea > QWidget > QWidget { background: transparent; }") + QStringLiteral("QScrollArea QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }") + QStringLiteral("QScrollArea QScrollBar::handle:vertical { background: ") + Theme::color(QStringLiteral("scrollbar")) + QStringLiteral("; border-radius: 4px; min-height: 30px; }") + QStringLiteral("QScrollArea QScrollBar::handle:vertical:hover { background: ") + Theme::color(QStringLiteral("scrollbarHover")) + QStringLiteral("; }") + QStringLiteral("QScrollArea QScrollBar::add-line:vertical, QScrollArea QScrollBar::sub-line:vertical { height: 0; }") + QStringLiteral("QScrollArea QScrollBar::add-page:vertical, QScrollArea QScrollBar::sub-page:vertical { background: transparent; }"));

	m_installedContainer = new QWidget;
	m_installedCardsLayout = new QVBoxLayout(m_installedContainer);
	m_installedCardsLayout->setContentsMargins(0, 0, 0, 0);
	m_installedCardsLayout->setSpacing(8);
	m_installedScroll->setWidget(m_installedContainer);

	installedLayout->addWidget(m_installedScroll, 1);

	m_tabs->addTab(installedPage, QStringLiteral("已安装"));

	// 安装进度
	m_progressBar = new QProgressBar(content);
	m_progressBar->setRange(0, 0);
	m_progressBar->setTextVisible(false);
	m_progressBar->setFixedHeight(8);
	m_progressBar->setStyleSheet(
		QStringLiteral("QProgressBar {")
		+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("inputBg")) + QStringLiteral(";")
		+ QStringLiteral("  border: none;")
		+ QStringLiteral("  border-radius: 4px;")
		+ QStringLiteral("}")
		+ QStringLiteral("QProgressBar::chunk {")
		+ QStringLiteral("  background: qlineargradient(x1:0, y1:0, x2:1, y2:0,")
		+ QStringLiteral("    stop:0 ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(",")
		+ QStringLiteral("    stop:0.5 ") + Theme::color(QStringLiteral("accentHover")) + QStringLiteral(",")
		+ QStringLiteral("    stop:1 ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(");")
		+ QStringLiteral("  border-radius: 4px;")
		+ QStringLiteral("}"));
	m_progressBar->hide();
	rootLayout->addWidget(m_progressBar);

	// 如果当前加载到的是快照，定时重新拉取，等后台刷新完成后自动切换到最新数据
	m_registryRefreshTimer = new QTimer(this);
	m_registryRefreshTimer->setInterval(3000);
	connect(m_registryRefreshTimer, &QTimer::timeout, this, [this]() {
		if (m_registryLoaded)
			loadRegistry();
		});

	// 状态栏
	m_statusLabel = new QLabel(QStringLiteral("正在加载插件市场..."), content);
	m_statusLabel->setStyleSheet(QStringLiteral("QLabel {") + QStringLiteral("  background: transparent;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textSecondary")) + QStringLiteral(";") + QStringLiteral("  font-size: 12px;") + QStringLiteral("}"));
	rootLayout->addWidget(m_statusLabel);

	setContent(content);

	resize(820, 600);

	connect(refreshButton, &QPushButton::clicked, this, &PluginsPopup::refresh);
	connect(m_restartButton, &QPushButton::clicked, this, &PluginsPopup::restartServer);
	connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() {
		m_currentPage = 0;
		populateMarket();
		});
	connect(m_prevButton, &QPushButton::clicked, this, [this]() { changePage(-1); });
	connect(m_nextButton, &QPushButton::clicked, this, [this]() { changePage(1); });

	refresh();
	checkAndInstallMarketIfNeeded();
}

void PluginsPopup::setBaseUrl(const QUrl& url)
{
	m_baseUrl = url;
}

void PluginsPopup::checkAndInstallMarketIfNeeded()
{
	const QString appDir = QCoreApplication::applicationDirPath();
	const QString marketPkg = appDir + QStringLiteral("/resources/server/harness/profiles/web/node_modules/dshmarket/package.json");
	if (QFile::exists(marketPkg))
		return;

	m_progressBar->show();
	m_statusLabel->setText(QStringLiteral("未检测到插件市场，正在自动安装..."));

	const QString nodePath = appDir + QStringLiteral("/resources/server/node.exe");
	const QString dshHome = appDir + QStringLiteral("/resources/server/harness");

	// 确保 pnpm 可用：如果 PATH 里没有 pnpm，就创建一个本地 shim
	const QString pnpmEntry = QDir::toNativeSeparators(appDir + QStringLiteral("/resources/server/node_modules/pnpm/bin/pnpm.cjs"));
	const QString binDir = QDir::toNativeSeparators(dshHome + QStringLiteral("/.desktop-bin"));
	QDir().mkpath(binDir);
	const QString pnpmCmdPath = binDir + QStringLiteral("/pnpm.cmd");
	if (!QFile::exists(pnpmCmdPath)) {
		QFile shim(pnpmCmdPath);
		if (shim.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
			shim.write(QStringLiteral("@echo off\r\n\"%1\" \"%2\" %*\r\n")
				.arg(QDir::toNativeSeparators(nodePath), pnpmEntry)
				.toUtf8());
			shim.close();
		}
	}

	m_installer = new QProcess(this);
	m_installer->setProcessChannelMode(QProcess::MergedChannels);

	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	env.insert(QStringLiteral("DSH_HOME"), dshHome);

	// 把本地 pnpm shim 目录加入 PATH
	QString path = env.value(QStringLiteral("Path"));
	if (!path.isEmpty())
		path = binDir + QLatin1Char(';') + path;
	else
		path = binDir;
	env.insert(QStringLiteral("Path"), path);
	env.insert(QStringLiteral("PATH"), path);

	m_installer->setProcessEnvironment(env);

	connect(m_installer, &QProcess::readyReadStandardOutput, this, [this]() {
		while (m_installer->canReadLine()) {
			const QString line = QString::fromUtf8(m_installer->readLine()).trimmed();
			if (line.isEmpty())
				continue;
			qInfo().noquote() << QStringLiteral("[market-installer]") << line;
			m_statusLabel->setText(QStringLiteral("正在安装插件市场: %1").arg(line.left(60)));
		}
		});

	const QString profileDir = QDir::toNativeSeparators(dshHome + QStringLiteral("/profiles/web"));

	connect(m_installer, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
		this, [this, profileDir](int exitCode, QProcess::ExitStatus exitStatus) {
			Q_UNUSED(exitStatus)
				m_progressBar->hide();
			m_installer->deleteLater();
			m_installer = nullptr;

			qInfo().noquote() << QStringLiteral("[market-installer] finished, exitCode=") << exitCode;

			if (exitCode == 0) {
				// pnpm 只写 dependencies，需要手动把 dshmarket 加入 profile bundles
				QFile manifestFile(profileDir + QStringLiteral("/package.json"));
				if (manifestFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
					QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll());
					manifestFile.close();
					if (doc.isObject()) {
						QJsonObject root = doc.object();
						QJsonObject dependencies = root.value(QStringLiteral("dependencies")).toObject();
						dependencies.insert(QStringLiteral("dshmarket"), QStringLiteral("1.9.0"));
						root.insert(QStringLiteral("dependencies"), dependencies);

						QJsonObject dsh = root.value(QStringLiteral("dsh")).toObject();
						QJsonObject profile = dsh.value(QStringLiteral("profile")).toObject();
						QJsonArray bundles = profile.value(QStringLiteral("bundles")).toArray();
						bool found = false;
						for (const auto& value : bundles) {
							if (value.toString() == QStringLiteral("dshmarket")) {
								found = true;
								break;
							}
						}
						if (!found)
							bundles.append(QStringLiteral("dshmarket"));
						profile.insert(QStringLiteral("bundles"), bundles);
						dsh.insert(QStringLiteral("profile"), profile);
						root.insert(QStringLiteral("dsh"), dsh);

						QFile out(profileDir + QStringLiteral("/package.json"));
						if (out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
							out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
							out.close();
						}
					}
				}

				m_statusLabel->setText(QStringLiteral("插件市场安装完成，正在重启服务..."));
				emit serverRestartRequested();
				retryRefreshAfterInstall(0);
			}
			else {
				m_statusLabel->setText(QStringLiteral("插件市场安装失败，请检查网络或稍后重试"));
			}
		});

	m_installer->setWorkingDirectory(profileDir);

	qInfo().noquote() << QStringLiteral("[market-installer] starting pnpm install for dshmarket@1.9.0");

	// 直接使用 node 运行 pnpm，避免依赖系统 PATH 里的 pnpm
	m_installer->start(nodePath, QStringList{
		pnpmEntry,
		QStringLiteral("add"),
		QStringLiteral("--save-exact"),
		QStringLiteral("dshmarket@1.9.0")
		});
}

void PluginsPopup::retryRefreshAfterInstall(int attempt)
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

void PluginsPopup::refresh()
{
	m_registryLoaded = false;
	m_statusLabel->setText(QStringLiteral("正在加载插件市场..."));
	loadRegistry();
	loadInstalled();
}

void PluginsPopup::loadRegistry()
{
	QNetworkRequest request(m_baseUrl);
	request.setUrl(QUrl(m_baseUrl.toString(QUrl::RemovePath) + QStringLiteral("/dsh-market/registry")));
	QNetworkReply* reply = m_nam->get(request);
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			qWarning().noquote() << "[PluginsPopup] registry request failed:" << reply->errorString();
			m_statusLabel->setText(QStringLiteral("加载插件市场失败: %1").arg(reply->errorString()));
			return;
		}
		const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
		const QString source = root.value(QStringLiteral("source")).toString();
		m_plugins = root.value(QStringLiteral("registry")).toObject()
			.value(QStringLiteral("plugins")).toArray();
		m_registryLoaded = true;
		populateMarket();

		qInfo().noquote() << "[PluginsPopup] registry loaded"
			<< "source=" << source
			<< "plugins=" << m_plugins.size();

		if (source == QStringLiteral("snapshot")) {
			// 快照不是最新数据，轮询直到后台刷新完成，让市场自动切换到最新列表
			if (m_registryRefreshTimer && !m_registryRefreshTimer->isActive())
				m_registryRefreshTimer->start();
			m_statusLabel->setText(QStringLiteral("插件市场已加载（离线快照），正在等待最新数据..."));
		}
		else {
			if (m_registryRefreshTimer)
				m_registryRefreshTimer->stop();
			if (source == QStringLiteral("cache")) {
				m_statusLabel->setText(QStringLiteral("插件市场已加载（缓存），共 %1 个插件").arg(m_plugins.size()));
			}
			else {
				m_statusLabel->setText(QStringLiteral("插件市场已加载，共 %1 个插件").arg(m_plugins.size()));
			}
		}
		});
}

void PluginsPopup::loadInstalled()
{
	QNetworkRequest request(m_baseUrl);
	request.setUrl(QUrl(m_baseUrl.toString(QUrl::RemovePath) + QStringLiteral("/dsh-market/installed")));
	QNetworkReply* reply = m_nam->get(request);
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError)
			return;
		const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
		m_installed = root.value(QStringLiteral("installed")).toObject();
		populateInstalled();
		});
}

void PluginsPopup::populateMarket()
{
	m_filteredPlugins = QJsonArray();
	const QString keyword = m_searchEdit->text().trimmed();

	for (const auto& value : m_plugins) {
		const QJsonObject plugin = value.toObject();
		const QString name = plugin.value(QStringLiteral("name")).toString();
		const QString category = plugin.value(QStringLiteral("category")).toString();
		const QJsonObject description = plugin.value(QStringLiteral("description")).toObject();
		const QString desc = description.value(QStringLiteral("zh")).toString();
		const QString descEn = description.value(QStringLiteral("en")).toString();

		if (!keyword.isEmpty()
			&& !name.contains(keyword, Qt::CaseInsensitive)
			&& !category.contains(keyword, Qt::CaseInsensitive)
			&& !desc.contains(keyword, Qt::CaseInsensitive)
			&& !descEn.contains(keyword, Qt::CaseInsensitive)) {
			continue;
		}

		m_filteredPlugins.append(plugin);
	}

	m_currentPage = 0;
	renderMarketPage();
}

void PluginsPopup::renderMarketPage()
{
	clearLayout(m_marketCardsLayout);

	const int total = m_filteredPlugins.size();
	const int pageCount = qMax(1, (total + m_pageSize - 1) / m_pageSize);
	if (m_currentPage >= pageCount)
		m_currentPage = pageCount - 1;
	if (m_currentPage < 0)
		m_currentPage = 0;

	const int start = m_currentPage * m_pageSize;
	const int end = qMin(start + m_pageSize, total);

	for (int i = start; i < end; ++i) {
		const QJsonObject plugin = m_filteredPlugins.at(i).toObject();
		m_marketCardsLayout->addWidget(createMarketCard(plugin));
	}

	m_marketCardsLayout->addStretch(1);

	m_pageLabel->setText(QStringLiteral("第 %1 / %2 页").arg(m_currentPage + 1).arg(pageCount));
	m_prevButton->setEnabled(m_currentPage > 0);
	m_nextButton->setEnabled(m_currentPage + 1 < pageCount);
}

void PluginsPopup::populateInstalled()
{
	clearLayout(m_installedCardsLayout);
	for (auto it = m_installed.begin(); it != m_installed.end(); ++it) {
		m_installedCardsLayout->addWidget(createInstalledCard(it.key(), it.value().toString()));
	}
	m_installedCardsLayout->addStretch(1);
}

void PluginsPopup::changePage(int delta)
{
	const int total = m_filteredPlugins.size();
	const int pageCount = qMax(1, (total + m_pageSize - 1) / m_pageSize);
	m_currentPage = qBound(0, m_currentPage + delta, pageCount - 1);
	renderMarketPage();
}

QWidget* PluginsPopup::createMarketCard(const QJsonObject& plugin)
{
	const QString name = plugin.value(QStringLiteral("name")).toString();
	const QString category = plugin.value(QStringLiteral("category")).toString();
	const QString url = plugin.value(QStringLiteral("url")).toString();
	const QJsonObject description = plugin.value(QStringLiteral("description")).toObject();
	const QString desc = description.value(QStringLiteral("zh")).toString();
	const QString descEn = description.value(QStringLiteral("en")).toString();
	const QString displayDesc = !desc.isEmpty() ? desc : descEn;

	auto* card = new QFrame;
	card->setObjectName(QStringLiteral("pluginCard"));
	card->setStyleSheet(QStringLiteral("QFrame#pluginCard {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";") + QStringLiteral("  border-radius: 12px;") + QStringLiteral("}") + QStringLiteral("QFrame#pluginCard:hover {") + QStringLiteral("  border-color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("inputBg")) + QStringLiteral(";") + QStringLiteral("}"));

	auto* layout = new QHBoxLayout(card);
	layout->setContentsMargins(14, 12, 14, 12);
	layout->setSpacing(12);

	auto* infoLayout = new QVBoxLayout;
	infoLayout->setSpacing(4);

	auto* titleRow = new QHBoxLayout;
	titleRow->setSpacing(8);

	auto* nameLabel = new QLabel(name, card);
	nameLabel->setStyleSheet(QStringLiteral("QLabel { background: transparent; color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral("; font-size: 14px; font-weight: 600; }"));

	auto* categoryLabel = new QLabel(category, card);
	categoryLabel->setStyleSheet(QStringLiteral("QLabel {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("tagBg")) + QStringLiteral(";") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";") + QStringLiteral("  border-radius: 6px;") + QStringLiteral("  padding: 2px 8px;") + QStringLiteral("  font-size: 11px;") + QStringLiteral("}"));

	titleRow->addWidget(nameLabel);
	if (!category.isEmpty())
		titleRow->addWidget(categoryLabel);
	titleRow->addStretch(1);

	infoLayout->addLayout(titleRow);

	if (!displayDesc.isEmpty()) {
		auto* descLabel = new QLabel(displayDesc, card);
		descLabel->setWordWrap(true);
		descLabel->setStyleSheet(QStringLiteral("QLabel { background: transparent; color: ") + Theme::color(QStringLiteral("textSecondary")) + QStringLiteral("; font-size: 12px; }"));
		infoLayout->addWidget(descLabel);
	}

	layout->addLayout(infoLayout, 1);

	auto* installButton = new QPushButton(QStringLiteral("安装"), card);
	installButton->setCursor(Qt::PointingHandCursor);
	installButton->setStyleSheet(QStringLiteral("QPushButton {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textOnAccent")) + QStringLiteral(";") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 10px;") + QStringLiteral("  padding: 8px 18px;") + QStringLiteral("  font-size: 13px;") + QStringLiteral("}") + QStringLiteral("QPushButton:hover {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("accentHover")) + QStringLiteral(";") + QStringLiteral("}"));
	connect(installButton, &QPushButton::clicked, this, [this, url]() {
		installPlugin(url);
		});

	layout->addWidget(installButton, 0, Qt::AlignVCenter);

	return card;
}

QWidget* PluginsPopup::createInstalledCard(const QString& name, const QString& version)
{
	auto* card = new QFrame;
	card->setObjectName(QStringLiteral("pluginCard"));
	card->setStyleSheet(QStringLiteral("QFrame#pluginCard {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";") + QStringLiteral("  border-radius: 12px;") + QStringLiteral("}"));

	auto* layout = new QHBoxLayout(card);
	layout->setContentsMargins(14, 12, 14, 12);
	layout->setSpacing(12);

	auto* nameLabel = new QLabel(
		version.isEmpty() ? name : QStringLiteral("%1  %2").arg(name, version),
		card);
	nameLabel->setStyleSheet(QStringLiteral("QLabel { background: transparent; color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral("; font-size: 14px; font-weight: 600; }"));

	layout->addWidget(nameLabel, 1);

	auto* updateButton = new QPushButton(QStringLiteral("更新"), card);
	auto* uninstallButton = new QPushButton(QStringLiteral("卸载"), card);

	for (QPushButton* button : { updateButton, uninstallButton }) {
		button->setCursor(Qt::PointingHandCursor);
		button->setStyleSheet(QStringLiteral("QPushButton {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg")) + QStringLiteral(";") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 10px;") + QStringLiteral("  padding: 8px 14px;") + QStringLiteral("  font-size: 12px;") + QStringLiteral("}") + QStringLiteral("QPushButton:hover {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";") + QStringLiteral("}"));
	}

	uninstallButton->setStyleSheet(QStringLiteral("QPushButton {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("dangerBg")) + QStringLiteral(";") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("danger")) + QStringLiteral(";") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 10px;") + QStringLiteral("  padding: 8px 14px;") + QStringLiteral("  font-size: 12px;") + QStringLiteral("}") + QStringLiteral("QPushButton:hover {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("dangerBg")) + QStringLiteral(";") + QStringLiteral("}"));

	connect(updateButton, &QPushButton::clicked, this, [this, name]() {
		updatePlugin(name);
		});
	connect(uninstallButton, &QPushButton::clicked, this, [this, name]() {
		uninstallPlugin(name);
		});

	layout->addWidget(updateButton);
	layout->addWidget(uninstallButton);

	return card;
}

void PluginsPopup::installPlugin(const QString& url)
{
	if (url.isEmpty())
		return;

	QJsonObject body;
	body.insert(QStringLiteral("url"), url);
	sendPost(QStringLiteral("/dsh-market/install"), body);
}

void PluginsPopup::uninstallPlugin(const QString& name)
{
	QJsonObject body;
	body.insert(QStringLiteral("name"), name);
	sendPost(QStringLiteral("/dsh-market/uninstall"), body);
}

void PluginsPopup::updatePlugin(const QString& name)
{
	QJsonObject body;
	body.insert(QStringLiteral("name"), name);
	sendPost(QStringLiteral("/dsh-market/update"), body);
}

void PluginsPopup::restartServer()
{
	sendPost(QStringLiteral("/dsh-market/restart"), QJsonObject());
}

void PluginsPopup::sendPost(const QString& path, const QJsonObject& body)
{
	QUrl url = m_baseUrl;
	url.setPath(path);
	QNetworkRequest request(url);
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	request.setRawHeader("Origin", m_baseUrl.toString(QUrl::RemovePath).toUtf8());

	QNetworkReply* reply = m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
			const QString error = root.value(QStringLiteral("error")).toString();
			m_statusLabel->setText(error.isEmpty() ? reply->errorString() : error);
			return;
		}
		m_statusLabel->setText(QStringLiteral("操作已提交，正在刷新..."));
		refresh();
		});
}