#pragma once

// ------------------------------------------------------------------
// ExtensionManagerPopup.h
// ------------------------------------------------------------------
// 扩展管理弹窗：
//   - 不直接弹出文件选择框，而是先打开管理窗口
//   - 窗口中可以安装 .ext 扩展、查看已安装扩展、移除扩展
//   - 继承 PopupWindow，样式与插件弹窗保持一致
// ------------------------------------------------------------------

#include "PopupWindow.h"
#include "ExtensionLoader.h"

#include <future>


#include <QString>
#include <QStringList>

class QLabel;
class QTimer;
class QListWidget;
class QPushButton;
class QVBoxLayout;

class ExtensionManagerPopup : public PopupWindow
{
    Q_OBJECT

public:
    explicit ExtensionManagerPopup(const QString &serverProfilePath,
                                   QWidget *parent = nullptr);

signals:
    void extensionInstalled(const QString &jsonPath, const QString &dllPath);
    void serverRestartRequested();

private slots:
    void installExtension();
    void pollInstall();
    void removeSelected();
    void refresh();

private:
    QString serverProfilePath() const;
    QString nodeModulesPath() const;
    QString registryPath() const;

    QStringList loadInstalledExtensions() const;
    void saveInstalledExtensions(const QStringList &names);

    void populateList();

    bool removeExtensionDirectory(const QString &name, QString *error);

    QString m_serverProfilePath;
    QListWidget *m_listWidget = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_installButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    std::future<bool> m_installFuture;
    ExtensionLoader::LoadedExtension m_pendingExt;
    QString m_pendingError;
    bool m_installing = false;
    QTimer *m_installTimer = nullptr;
};
