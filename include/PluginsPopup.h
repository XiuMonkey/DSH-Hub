#pragma once

#include "PopupWindow.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

class QLineEdit;
class QScrollArea;
class QLabel;
class QPushButton;
class QNetworkAccessManager;
class QTabWidget;
class QVBoxLayout;
class QProgressBar;
class QProcess;

class PluginsPopup : public PopupWindow
{
    Q_OBJECT

public:
    explicit PluginsPopup(const QUrl &baseUrl, QWidget *parent = nullptr);

private slots:
    void refresh();
    void installPlugin(const QString &url);
    void uninstallPlugin(const QString &name);
    void updatePlugin(const QString &name);
    void restartServer();
    void changePage(int delta);

private:
    void checkAndInstallMarketIfNeeded();
    void loadRegistry();
    void loadInstalled();
    void populateMarket();
    void populateInstalled();
    void renderMarketPage();
    void sendPost(const QString &path, const QJsonObject &body);
    QWidget *createMarketCard(const QJsonObject &plugin);
    QWidget *createInstalledCard(const QString &name, const QString &version);

    QUrl m_baseUrl;
    QNetworkAccessManager *m_nam = nullptr;
    QTabWidget *m_tabs = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QScrollArea *m_marketScroll = nullptr;
    QWidget *m_marketContainer = nullptr;
    QVBoxLayout *m_marketCardsLayout = nullptr;
    QScrollArea *m_installedScroll = nullptr;
    QWidget *m_installedContainer = nullptr;
    QVBoxLayout *m_installedCardsLayout = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QProcess *m_installer = nullptr;
    QPushButton *m_prevButton = nullptr;
    QPushButton *m_nextButton = nullptr;
    QLabel *m_pageLabel = nullptr;
    QPushButton *m_restartButton = nullptr;

    QJsonArray m_plugins;
    QJsonArray m_filteredPlugins;
    QJsonObject m_installed;
    int m_currentPage = 0;
    int m_pageSize = 20;
};