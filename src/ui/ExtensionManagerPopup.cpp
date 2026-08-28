#include "ExtensionManagerPopup.h"
#include "ExtensionLoader.h"
#include "ThemeManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QAbstractItemView>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QTimer>

#include <chrono>
namespace
{

QString patchNameLine(const QString &name)
{
    return QStringLiteral("      name: '%1'").arg(name);
}

QString patchIdLine(const QString &name)
{
    return QStringLiteral("    - id: %1").arg(name);
}

bool removePatchEntryForExtension(const QString &profilePath, const QString &name)
{
    const QString patchPath = profilePath + QStringLiteral("/cordis.patch.yml");
    QFile file(patchPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QString text = QString::fromUtf8(file.readAll());
    file.close();

    const QStringList lines = text.split(QLatin1Char('\n'));
    QStringList kept;
    bool removed = false;

    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i);
        const QString trimmed = line.trimmed();

        // Remove the standard two-line entry added by ExtensionLoader:
        //     - id: <name>
        //       name: '<name>'
        if (trimmed == patchIdLine(name) && i + 1 < lines.size()
            && lines.at(i + 1).trimmed() == patchNameLine(name)) {
            ++i; // skip the next line too
            removed = true;
            continue;
        }

        // Also remove a standalone name line if it somehow exists without the id pair.
        if (trimmed == patchNameLine(name)) {
            removed = true;
            continue;
        }

        kept.append(line);
    }

    if (!removed)
        return true;

    // Remove empty "- insert:" headers left behind after deleting the entry lines.
    QStringList cleaned;
    for (int i = 0; i < kept.size(); ++i) {
        if (kept.at(i).trimmed() == QStringLiteral("- insert:")) {
            int j = i + 1;
            while (j < kept.size() && kept.at(j).trimmed().isEmpty())
                ++j;
            if (j >= kept.size() || kept.at(j).trimmed().startsWith(QStringLiteral("- insert:"))) {
                continue;
            }
        }
        cleaned.append(kept.at(i));
    }
    const QString newText = cleaned.join(QLatin1Char('\n'));

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return false;

    file.write(newText.toUtf8());
    file.close();
    return true;
}

} // namespace

ExtensionManagerPopup::ExtensionManagerPopup(const QString &serverProfilePath, QWidget *parent)
    : PopupWindow(parent)
    , m_serverProfilePath(serverProfilePath)
{
    setTitle(QStringLiteral("扩展管理"));
    qInfo().noquote() << QStringLiteral("[ExtensionManager] popup created");

    auto *content = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(content);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

    // 顶部按钮栏
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);

    m_installButton = new QPushButton(QStringLiteral("安装扩展"), content);
    m_removeButton = new QPushButton(QStringLiteral("移除选中"), content);
    m_refreshButton = new QPushButton(QStringLiteral("刷新"), content);

    const QString buttonStyle = QStringLiteral("QPushButton {")
        + QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg"))
        + QStringLiteral(";")
        + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary"))
        + QStringLiteral(";")
        + QStringLiteral("  border: none;")
        + QStringLiteral("  border-radius: 10px;")
        + QStringLiteral("  padding: 8px 14px;")
        + QStringLiteral("  font-size: 13px;")
        + QStringLiteral("}")
        + QStringLiteral("QPushButton:hover {")
        + QStringLiteral("  background: ") + Theme::color(QStringLiteral("border"))
        + QStringLiteral(";")
        + QStringLiteral("}")
        + QStringLiteral("QPushButton:disabled {")
        + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textSecondary"))
        + QStringLiteral(";")
        + QStringLiteral("  background: ") + Theme::color(QStringLiteral("inputBg"))
        + QStringLiteral(";")
        + QStringLiteral("}");

    for (QPushButton *button : {m_installButton, m_removeButton, m_refreshButton}) {
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(buttonStyle);
    }

    toolbar->addWidget(m_installButton);
    toolbar->addWidget(m_removeButton);
    toolbar->addStretch(1);
    toolbar->addWidget(m_refreshButton);

    rootLayout->addLayout(toolbar);

    // 扩展列表
    m_listWidget = new QListWidget(content);
    m_listWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listWidget->setStyleSheet(QStringLiteral("QListWidget {")
        + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg"))
        + QStringLiteral(";")
        + QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("border"))
        + QStringLiteral(";")
        + QStringLiteral("  border-radius: 12px;")
        + QStringLiteral("  padding: 8px;")
        + QStringLiteral("  font-size: 13px;")
        + QStringLiteral("}")
        + QStringLiteral("QListWidget::item {")
        + QStringLiteral("  padding: 8px 10px;")
        + QStringLiteral("  border-radius: 8px;")
        + QStringLiteral("}")
        + QStringLiteral("QListWidget::item:selected {")
        + QStringLiteral("  background: ") + Theme::color(QStringLiteral("activeBg"))
        + QStringLiteral(";")
        + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary"))
        + QStringLiteral(";")
        + QStringLiteral("}")
        + QStringLiteral("QListWidget::item:hover {")
        + QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg"))
        + QStringLiteral(";")
        + QStringLiteral("}"));

    rootLayout->addWidget(m_listWidget, 1);

    // 状态栏
    m_statusLabel = new QLabel(content);
    m_statusLabel->setStyleSheet(QStringLiteral("QLabel {")
        + QStringLiteral("  background: transparent;")
        + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textSecondary"))
        + QStringLiteral(";")
        + QStringLiteral("  font-size: 12px;")
        + QStringLiteral("}"));

    rootLayout->addWidget(m_statusLabel);

    setContent(content);
    resize(720, 480);

    connect(m_installButton, &QPushButton::clicked, this, &ExtensionManagerPopup::installExtension);
    connect(m_removeButton, &QPushButton::clicked, this, &ExtensionManagerPopup::removeSelected);
    connect(m_refreshButton, &QPushButton::clicked, this, &ExtensionManagerPopup::refresh);
    connect(m_listWidget, &QListWidget::itemSelectionChanged, this, [this]() {
        m_removeButton->setEnabled(m_listWidget->currentItem() != nullptr);
    });

    m_installTimer = new QTimer(this);
    m_installTimer->setInterval(100);
    connect(m_installTimer, &QTimer::timeout, this, &ExtensionManagerPopup::pollInstall);

    // 关闭弹窗时等待异步安装任务结束，避免任务继续写已销毁的 this
    connect(this, &PopupWindow::closed, this, [this]() {
        if (m_installFuture.valid())
            m_installFuture.wait();
    });

    refresh();
}

QString ExtensionManagerPopup::serverProfilePath() const
{
    return m_serverProfilePath;
}

QString ExtensionManagerPopup::nodeModulesPath() const
{
    return m_serverProfilePath + QStringLiteral("/node_modules");
}

QString ExtensionManagerPopup::registryPath() const
{
    return m_serverProfilePath + QStringLiteral("/extensions.json");
}

QStringList ExtensionManagerPopup::loadInstalledExtensions() const
{
    QStringList names;

    const QString path = registryPath();
    if (!QFile::exists(path))
        return names;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return names;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (doc.isArray()) {
        const QJsonArray array = doc.array();
        for (const auto &value : array) {
            const QString name = value.toString();
            if (!name.isEmpty())
                names.append(name);
        }
    }

    return names;
}

void ExtensionManagerPopup::saveInstalledExtensions(const QStringList &names)
{
    QJsonArray array;
    for (const QString &name : names)
        array.append(name);

    QFile file(registryPath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
        file.close();
    }
}

void ExtensionManagerPopup::populateList()
{
    m_listWidget->clear();

    const QStringList names = loadInstalledExtensions();
    for (const QString &name : names)
        m_listWidget->addItem(name);

    if (names.isEmpty()) {
        m_statusLabel->setText(QStringLiteral("暂无已安装扩展，点击“安装扩展”选择 .ext 文件。"));
    } else {
        m_statusLabel->setText(QStringLiteral("已安装 %1 个扩展。").arg(names.size()));
    }

    m_removeButton->setEnabled(false);
}
void ExtensionManagerPopup::installExtension()
{
    if (m_installing)
        return;

    const QString extPath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择 DSH 扩展"),
        QString(),
        QStringLiteral("DSH Extension (*.ext)"));

    if (extPath.isEmpty())
        return;
    qInfo().noquote() << QStringLiteral("[ExtensionManager] installExtension start: %1").arg(extPath);

    m_installing = true;
    if (m_installButton)
        m_installButton->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("正在安装扩展..."));
    m_pendingError.clear();
    m_pendingExt = ExtensionLoader::LoadedExtension();

    const QString profilePath = m_serverProfilePath;
    m_installFuture = std::async(std::launch::async, [this, extPath, profilePath]() {
        ExtensionLoader loader;
        return loader.loadAndInstall(extPath, profilePath, &m_pendingExt, &m_pendingError);
    });

    if (m_installTimer)
        m_installTimer->start();
    qInfo().noquote() << QStringLiteral("[ExtensionManager] async install started, timer running");
}

void ExtensionManagerPopup::pollInstall()
{
    if (!m_installFuture.valid() ||
        m_installFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    qInfo().noquote() << QStringLiteral("[ExtensionManager] pollInstall: future ready");

    if (m_installTimer)
        m_installTimer->stop();
    m_installing = false;
    if (m_installButton)
        m_installButton->setEnabled(true);

    const bool ok = m_installFuture.get();
    if (!ok) {
        m_statusLabel->setText(QStringLiteral("扩展安装失败: %1").arg(m_pendingError));
        return;
    }
    qInfo().noquote() << QStringLiteral("[ExtensionManager] loadAndInstall returned ok=%1").arg(ok);

    QStringList names = loadInstalledExtensions();
    if (!names.contains(m_pendingExt.pluginName)) {
        names.append(m_pendingExt.pluginName);
        saveInstalledExtensions(names);
    }
    qInfo().noquote() << QStringLiteral("[ExtensionManager] installed registry updated");

    populateList();
    m_statusLabel->setText(QStringLiteral("扩展安装成功: %1").arg(m_pendingExt.pluginName));
    qInfo().noquote() << QStringLiteral("[ExtensionManager] list populated, emitting extensionInstalled");

    // 使用持久化到扩展目录的 regulation.json5 / main.dll，
    // 避免依赖临时解压目录，重启后也能从同一位置加载。
    if (!m_pendingExt.pluginName.isEmpty()) {
        const QString extDir = m_serverProfilePath
            + QStringLiteral("/extensions/") + m_pendingExt.pluginName;
        m_pendingExt.jsonPath = extDir + QStringLiteral("/regulation.json5");
        m_pendingExt.dllPath = extDir + QStringLiteral("/main.dll");
    }

    emit extensionInstalled(m_pendingExt.jsonPath, m_pendingExt.dllPath);
    qInfo().noquote() << QStringLiteral("[ExtensionManager] extensionInstalled emitted, emitting serverRestartRequested");
    emit serverRestartRequested();
}

void ExtensionManagerPopup::removeSelected()
{
    QListWidgetItem *item = m_listWidget->currentItem();
    if (!item)
        return;

    const QString name = item->text();

    if (QMessageBox::question(
            this,
            QStringLiteral("确认移除"),
            QStringLiteral("确定要移除扩展“%1”吗？").arg(name),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    QString error;
    if (!removeExtensionDirectory(name, &error)) {
        m_statusLabel->setText(error);
        return;
    }

    if (!removePatchEntryForExtension(m_serverProfilePath, name)) {
        m_statusLabel->setText(QStringLiteral("扩展目录已删除，但更新 cordis.patch.yml 失败: %1").arg(name));
    }

    QStringList names = loadInstalledExtensions();
    names.removeAll(name);
    saveInstalledExtensions(names);

    populateList();
    m_statusLabel->setText(QStringLiteral("已移除扩展: %1").arg(name));

    emit serverRestartRequested();
}

void ExtensionManagerPopup::refresh()
{
    populateList();
}

bool ExtensionManagerPopup::removeExtensionDirectory(const QString &name, QString *error)
{
    QDir dir(nodeModulesPath() + QStringLiteral("/") + name);
    if (dir.exists() && !dir.removeRecursively()) {
        if (error)
            *error = QStringLiteral("无法删除扩展目录: %1").arg(dir.absolutePath());
        return false;
    }

    QDir extDir(m_serverProfilePath + QStringLiteral("/extensions/") + name);
    if (extDir.exists() && !extDir.removeRecursively()) {
        if (error)
            *error = QStringLiteral("无法删除扩展资源目录: %1").arg(extDir.absolutePath());
        return false;
    }

    return true;
}

