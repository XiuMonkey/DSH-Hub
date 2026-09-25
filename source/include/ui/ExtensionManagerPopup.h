#pragma once

// 扩展管理弹窗：不直接弹文件选择框，而是先开管理窗口，在其中安装 .ext、查看已安装、移除扩展；
// 继承 StatusPopupWindow 以与插件弹窗样式一致。与界面无关的逻辑都在 common：extensions.json /
// 扩展目录 / cordis.patch.yml -> ExtensionRegistry，后台解压与安装 -> ExtensionInstallTask。

#include "ui/StatusPopupWindow.h"

#include "common/extension/ExtensionInstallTask.h"
#include "common/extension/ExtensionRegistry.h"

#include <QString>

class QLabel;
class QTimer;
class QListWidget;
class QPushButton;
class QVBoxLayout;

class ExtensionManagerPopup : public StatusPopupWindow
{
	Q_OBJECT

public:
	explicit ExtensionManagerPopup(const QString& serverProfilePath, QWidget* parent = nullptr);

	// 清理 cordis.patch.yml 中指向不存在包的残留条目
	void cleanupResiduals();

signals:
	void extensionInstalled(const QString& jsonPath, const QString& dllPath);
	void extensionRemoving(const QString& name);
	void serverRestartRequested();

private slots:
	void installExtension();
	void pollInstall();
	void removeSelected();
	void refresh();

private:
	// 把 ExtensionRegistry 里的已安装清单画到列表控件
	void populateList();

	ExtensionRegistry m_registry;
	ExtensionInstallTask m_installTask;

	QListWidget* m_listWidget = nullptr;
	QLabel* m_statusLabel = nullptr;
	QPushButton* m_installButton = nullptr;
	QPushButton* m_removeButton = nullptr;
	QPushButton* m_refreshButton = nullptr;
	QTimer* m_installTimer = nullptr;
};
