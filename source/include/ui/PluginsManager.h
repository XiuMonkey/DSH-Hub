#pragma once

#include "common/extension/PluginMarketModel.h"
#include "ui/StatusPopupWindow.h"

#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QVector>

class QLineEdit;
class QScrollArea;
class QLabel;
class QPushButton;
class QTabWidget;
class QVBoxLayout;
class QProgressBar;
class QTimer;
class PluginMarketClient;
class PluginMarketInstaller;

// “插件管理”界面：与 Settings 同思路的常驻对象，管插件市场/已安装列表的绘制与交互，外加窗口本身的开关（遮罩、居中、判重、关闭清理）。
// 与界面无关的逻辑都在 common：HTTP -> PluginMarketClient，市场包缺失时的自动安装（pnpm + profile 清单）-> PluginMarketInstaller，
// registry JSON 解析 / 关键词过滤 / 分页切片 -> PluginMarketModel；遮罩是窗口级共用的那一层，打开时申请、关闭时归还。
class PluginsManager : public StatusPopupWindow
{
	Q_OBJECT

public:
	// baseUrl = DSH 服务地址（后续可经 setBaseUrl 更新）；host = 宿主主窗口（用于定位遮罩与居中）
	explicit PluginsManager(const QUrl& baseUrl, QWidget* host);

	void setBaseUrl(const QUrl& url);

	// 打开插件管理窗口（幂等：已打开则直接返回；打开前刷新数据）
	void openPlugins();

	// 关闭插件管理窗口并归还遮罩（幂等）
	void closePlugins();

signals:
	void serverRestartRequested();

private slots:
	void refresh();
	void changePage(int delta);

private:
	// 打开前刷新数据（市场 + 已安装 + 确保市场包存在）
	void refreshOnOpen() override;

	// 市场包安装完成后，等后台刷新出结果前轮询几次
	void retryRefreshAfterInstall(int attempt);

	void onInstalledLoaded(const QJsonObject& installed);
	void onMarketOperationCompleted(const QString& path);
	void onMarketOperationFailed(const QString& path, const QString& error, int status);

	void populateMarket();
	void renderMarketPage();
	void populateInstalled();

	QWidget* createMarketCard(const MarketPlugin& plugin);
	QWidget* createInstalledCard(const QString& name, const QString& version);

	PluginMarketClient* m_market = nullptr;
	PluginMarketInstaller* m_installer = nullptr;

	QTabWidget* m_tabs = nullptr;
	QLineEdit* m_searchEdit = nullptr;
	QScrollArea* m_marketScroll = nullptr;
	QWidget* m_marketContainer = nullptr;
	QVBoxLayout* m_marketCardsLayout = nullptr;
	QScrollArea* m_installedScroll = nullptr;
	QWidget* m_installedContainer = nullptr;
	QVBoxLayout* m_installedCardsLayout = nullptr;
	QLabel* m_statusLabel = nullptr;
	QProgressBar* m_progressBar = nullptr;
	QTimer* m_registryRefreshTimer = nullptr;
	QPushButton* m_prevButton = nullptr;
	QPushButton* m_nextButton = nullptr;
	QLabel* m_pageLabel = nullptr;
	QPushButton* m_restartButton = nullptr;

	QVector<MarketPlugin> m_plugins;
	QVector<MarketPlugin> m_filteredPlugins;
	QJsonObject m_installed;
	bool m_registryLoaded = false;
	int m_currentPage = 0;
	int m_pageSize = 20;
};
