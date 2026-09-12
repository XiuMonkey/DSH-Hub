#pragma once

#include "PluginMarketModel.h"
#include "StatusPopupWindow.h"

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

// ------------------------------------------------------------------
// PluginsManager —— “插件管理”界面（不是一次性的窗口实例）
// ------------------------------------------------------------------
// 与 Settings 同一思路：PluginsManager 是随主窗口（DSHHub）创建后一直
// 存在的常驻对象，负责：
//   1) 插件市场 / 已安装列表的绘制与交互（卡片、分页、状态栏）；
//   2) 窗口本身的开关管理：灰色遮罩、居中、判重、关闭清理。
//
// 与界面无关的逻辑都在 common：
//   - HTTP（registry/installed/install/uninstall/update/restart）
//       -> PluginMarketClient
//   - 市场包缺失时的自动安装（pnpm + profile 清单）
//       -> PluginMarketInstaller
//   - registry JSON 解析、关键词过滤、分页切片
//       -> PluginMarketModel
//
// 打开/关闭统一走 openPlugins() / closePlugins()：
//   - 主窗口只保留少量调用（把侧边栏 pluginsRequested 信号直接连到
//     openPlugins()），不再在 DSHHub 里管理遮罩成员；
//   - 遮罩是本类自建的子控件，宿主 resize 时通过 syncOverlayToHost()
//     保持铺满（由主窗口 resizeEvent 调用）；
//   - 每次打开时 refreshOnOpen() 重新拉取市场/已安装数据，
//     避免常驻对象在服务端未就绪时就联网请求；
//   - 右上角 ✕ 关闭时自动清理遮罩并隐藏（closed -> closePlugins()）。
// ------------------------------------------------------------------
class PluginsManager : public StatusPopupWindow
{
	Q_OBJECT

public:
	// baseUrl：DSH 服务地址（后续可经 setBaseUrl 更新）
	// host   ：宿主主窗口（用于定位遮罩与居中）
	explicit PluginsManager(const QUrl& baseUrl, QWidget* host);

	void setBaseUrl(const QUrl& url);

	// 打开插件管理窗口（幂等：已打开则直接返回；打开前刷新数据）
	void openPlugins();

	// 关闭插件管理窗口并清理遮罩（幂等）
	void closePlugins();

	// 宿主窗口 resize 后调用，让遮罩重新铺满宿主（未打开时为空操作）
	void syncOverlayToHost();

signals:
	void serverRestartRequested();

private slots:
	void refresh();
	void changePage(int delta);

private:
	// 打开前刷新数据（市场 + 已安装 + 确保市场包存在）
	void refreshOnOpen();

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

	QWidget* m_host = nullptr;          // 宿主主窗口（遮罩/居中定位）
	QWidget* m_overlay = nullptr;       // 本类自管的灰色遮罩
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
