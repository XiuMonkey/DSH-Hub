#include "PluginsPopup.h"
#include "ThemeManager.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>

namespace
{
void clearLayout(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
}
}

PluginsPopup::PluginsPopup(const QUrl &baseUrl, QWidget *parent)
    : PopupWindow(parent)
    , m_baseUrl(baseUrl)
    , m_nam(new QNetworkAccessManager(this))
{
    setTitle(QStringLiteral("插件市场"));

    auto *content = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(content);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

    // 顶部工具栏
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);

    m_searchEdit = new QLineEdit(content);
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索插件..."));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setStyleSheet(QStringLiteral("QLineEdit {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("inputBg")) + QStringLiteral(";") + QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";") + QStringLiteral("  border-radius: 10px;") + QStringLiteral("  padding: 8px 12px;") + QStringLiteral("  font-size: 13px;") + QStringLiteral("}") + QStringLiteral("QLineEdit:focus {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border-color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";") + QStringLiteral("}"));

    auto *refreshButton = new QPushButton(QStringLiteral("刷新"), content);
    m_restartButton = new QPushButton(QStringLiteral("重启服务"), content);

    for (QPushButton *button : {refreshButton, m_restartButton}) {
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
    auto *marketPage = new QWidget(content);
    auto *marketLayout = new QVBoxLayout(marketPage);
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
    auto *pageBar = new QHBoxLayout;
    m_prevButton = new QPushButton(QStringLiteral("上一页"), marketPage);
    m_nextButton = new QPushButton(QStringLiteral("下一页"), marketPage);
    m_pageLabel = new QLabel(QStringLiteral("第 1 / 1 页"), marketPage);
    m_pageLabel->setAlignment(Qt::AlignCenter);
    m_pageLabel->setStyleSheet(QStringLiteral("QLabel { color: ") + Theme::color(QStringLiteral("textSecondary")) + QStringLiteral("; font-size: 12px; background: transparent; }"));

    for (QPushButton *button : {m_prevButton, m_nextButton}) {
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
    auto *installedPage = new QWidget(content);
    auto *installedLayout = new QVBoxLayout(installedPage);
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
}

void PluginsPopup::refresh()
{
    m_statusLabel->setText(QStringLiteral("正在加载插件市场..."));
    loadRegistry();
    loadInstalled();
}

void PluginsPopup::loadRegistry()
{
    QNetworkRequest request(m_baseUrl);
    request.setUrl(QUrl(m_baseUrl.toString(QUrl::RemovePath) + QStringLiteral("/dsh-market/registry")));
    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            m_statusLabel->setText(QStringLiteral("加载插件市场失败: %1").arg(reply->errorString()));
            return;
        }
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        m_plugins = root.value(QStringLiteral("registry")).toObject()
                        .value(QStringLiteral("plugins")).toArray();
        populateMarket();
        m_statusLabel->setText(QStringLiteral("插件市场已加载，共 %1 个插件").arg(m_plugins.size()));
    });
}

void PluginsPopup::loadInstalled()
{
    QNetworkRequest request(m_baseUrl);
    request.setUrl(QUrl(m_baseUrl.toString(QUrl::RemovePath) + QStringLiteral("/dsh-market/installed")));
    QNetworkReply *reply = m_nam->get(request);
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

    for (const auto &value : m_plugins) {
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

QWidget *PluginsPopup::createMarketCard(const QJsonObject &plugin)
{
    const QString name = plugin.value(QStringLiteral("name")).toString();
    const QString category = plugin.value(QStringLiteral("category")).toString();
    const QString url = plugin.value(QStringLiteral("url")).toString();
    const QJsonObject description = plugin.value(QStringLiteral("description")).toObject();
    const QString desc = description.value(QStringLiteral("zh")).toString();
    const QString descEn = description.value(QStringLiteral("en")).toString();
    const QString displayDesc = !desc.isEmpty() ? desc : descEn;

    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("pluginCard"));
    card->setStyleSheet(QStringLiteral("QFrame#pluginCard {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";") + QStringLiteral("  border-radius: 12px;") + QStringLiteral("}") + QStringLiteral("QFrame#pluginCard:hover {") + QStringLiteral("  border-color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("inputBg")) + QStringLiteral(";") + QStringLiteral("}"));

    auto *layout = new QHBoxLayout(card);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(12);

    auto *infoLayout = new QVBoxLayout;
    infoLayout->setSpacing(4);

    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);

    auto *nameLabel = new QLabel(name, card);
    nameLabel->setStyleSheet(QStringLiteral("QLabel { background: transparent; color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral("; font-size: 14px; font-weight: 600; }"));

    auto *categoryLabel = new QLabel(category, card);
    categoryLabel->setStyleSheet(QStringLiteral("QLabel {") + QStringLiteral("  background: ") + QStringLiteral("#EEF2FF") + QStringLiteral(";") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";") + QStringLiteral("  border-radius: 6px;") + QStringLiteral("  padding: 2px 8px;") + QStringLiteral("  font-size: 11px;") + QStringLiteral("}"));

    titleRow->addWidget(nameLabel);
    if (!category.isEmpty())
        titleRow->addWidget(categoryLabel);
    titleRow->addStretch(1);

    infoLayout->addLayout(titleRow);

    if (!displayDesc.isEmpty()) {
        auto *descLabel = new QLabel(displayDesc, card);
        descLabel->setWordWrap(true);
        descLabel->setStyleSheet(QStringLiteral("QLabel { background: transparent; color: ") + Theme::color(QStringLiteral("textSecondary")) + QStringLiteral("; font-size: 12px; }"));
        infoLayout->addWidget(descLabel);
    }

    layout->addLayout(infoLayout, 1);

    auto *installButton = new QPushButton(QStringLiteral("安装"), card);
    installButton->setCursor(Qt::PointingHandCursor);
    installButton->setStyleSheet(QStringLiteral("QPushButton {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";") + QStringLiteral("  color: white;") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 10px;") + QStringLiteral("  padding: 8px 18px;") + QStringLiteral("  font-size: 13px;") + QStringLiteral("}") + QStringLiteral("QPushButton:hover {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("accentHover")) + QStringLiteral(";") + QStringLiteral("}"));
    connect(installButton, &QPushButton::clicked, this, [this, url]() {
        installPlugin(url);
    });

    layout->addWidget(installButton, 0, Qt::AlignVCenter);

    return card;
}

QWidget *PluginsPopup::createInstalledCard(const QString &name, const QString &version)
{
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("pluginCard"));
    card->setStyleSheet(QStringLiteral("QFrame#pluginCard {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";") + QStringLiteral("  border-radius: 12px;") + QStringLiteral("}"));

    auto *layout = new QHBoxLayout(card);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(12);

    auto *nameLabel = new QLabel(
        version.isEmpty() ? name : QStringLiteral("%1  %2").arg(name, version),
        card);
    nameLabel->setStyleSheet(QStringLiteral("QLabel { background: transparent; color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral("; font-size: 14px; font-weight: 600; }"));

    layout->addWidget(nameLabel, 1);

    auto *updateButton = new QPushButton(QStringLiteral("更新"), card);
    auto *uninstallButton = new QPushButton(QStringLiteral("卸载"), card);

    for (QPushButton *button : {updateButton, uninstallButton}) {
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

void PluginsPopup::installPlugin(const QString &url)
{
    if (url.isEmpty())
        return;

    QJsonObject body;
    body.insert(QStringLiteral("url"), url);
    sendPost(QStringLiteral("/dsh-market/install"), body);
}

void PluginsPopup::uninstallPlugin(const QString &name)
{
    QJsonObject body;
    body.insert(QStringLiteral("name"), name);
    sendPost(QStringLiteral("/dsh-market/uninstall"), body);
}

void PluginsPopup::updatePlugin(const QString &name)
{
    QJsonObject body;
    body.insert(QStringLiteral("name"), name);
    sendPost(QStringLiteral("/dsh-market/update"), body);
}

void PluginsPopup::restartServer()
{
    sendPost(QStringLiteral("/dsh-market/restart"), QJsonObject());
}

void PluginsPopup::sendPost(const QString &path, const QJsonObject &body)
{
    QUrl url = m_baseUrl;
    url.setPath(path);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Origin", m_baseUrl.toString(QUrl::RemovePath).toUtf8());

    QNetworkReply *reply = m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
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