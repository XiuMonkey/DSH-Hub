#include "ui/ExtensionManagerPopup.h"
#include "core/ConnectionManager.h"
#include "ExtensionSystem/ClientExtension.h"

#include <QAbstractItemView>
#include <QDebug>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
	// 条目元数据：显示文本带种类后缀，移除时必须用这里的真名
	constexpr int kExtKindRole = Qt::UserRole;
	constexpr int kExtNameRole = Qt::UserRole + 1;
}

ExtensionManagerPopup::ExtensionManagerPopup(const QString& serverProfilePath, QWidget* parent)
	: StatusPopupWindow(parent)
	, m_registry(serverProfilePath)
{
	setTitle(qtTrId("ext_manager_title"));
	qInfo().noquote() << QStringLiteral("[ExtensionManager] popup created");

	auto* content = new QWidget(this);
	auto* rootLayout = new QVBoxLayout(content);
	rootLayout->setContentsMargins(0, 0, 0, 0);
	rootLayout->setSpacing(10);

	auto* toolbar = new QHBoxLayout;
	toolbar->setSpacing(8);

	m_installButton = new QPushButton(qtTrId("ext_install"), content);
	m_installButton->setObjectName(QStringLiteral("extPopupInstallButton"));
	m_removeButton = new QPushButton(qtTrId("ext_remove_selected"), content);
	m_removeButton->setObjectName(QStringLiteral("extPopupRemoveButton"));
	m_refreshButton = new QPushButton(qtTrId("common_refresh"), content);
	m_refreshButton->setObjectName(QStringLiteral("extPopupRefreshButton"));

	for (QPushButton* button : { m_installButton, m_removeButton, m_refreshButton }) {
		button->setCursor(Qt::PointingHandCursor);
	}

	toolbar->addWidget(m_installButton);
	toolbar->addWidget(m_removeButton);
	toolbar->addStretch(1);
	toolbar->addWidget(m_refreshButton);

	rootLayout->addLayout(toolbar);

	m_listWidget = new QListWidget(content);
	m_listWidget->setObjectName(QStringLiteral("extPopupList"));
	m_listWidget->setSelectionMode(QAbstractItemView::SingleSelection);

	rootLayout->addWidget(m_listWidget, 1);

	m_statusLabel = new QLabel(content);
	m_statusLabel->setObjectName(QStringLiteral("extPopupStatus"));
	attachStatusLabel(m_statusLabel);

	rootLayout->addWidget(m_statusLabel);

	setContent(content);
	resize(720, 480);

	dshRegister("ExtensionManagerPopup.001", m_installButton, qOverload<bool>(&QPushButton::clicked), this,
		&ExtensionManagerPopup::installExtension);
	dshRegister("ExtensionManagerPopup.002", m_removeButton, qOverload<bool>(&QPushButton::clicked), this,
		&ExtensionManagerPopup::removeSelected);
	dshRegister("ExtensionManagerPopup.003", m_refreshButton, qOverload<bool>(&QPushButton::clicked), this,
		&ExtensionManagerPopup::refresh);
	dshRegister("ExtensionManagerPopup.004", m_listWidget, &QListWidget::itemSelectionChanged, this, [this]() {
			m_removeButton->setEnabled(m_listWidget->currentItem() != nullptr);
		});

	m_installTimer = new QTimer(this);
	m_installTimer->setInterval(100);
	dshRegister("ExtensionManagerPopup.005", m_installTimer, &QTimer::timeout, this,
		&ExtensionManagerPopup::pollInstall);

	// 关闭弹窗前等异步安装结束，避免任务继续写已销毁的 this
	dshRegister("ExtensionManagerPopup.006", this, &PopupWindow::closed, this, [this]() {
			m_installTask.waitForFinished();
		});

	refresh();
}

void ExtensionManagerPopup::populateList()
{
	m_listWidget->clear();

	// 两种扩展数据源不同：工具扩展来自 extensions.json，客户端扩展来自 <exe>/clientExtensions/
	const QStringList toolNames = m_registry.installedExtensions();
	for (const QString& name : toolNames) {
		auto* item = new QListWidgetItem(name);
		item->setData(kExtKindRole, QStringLiteral("tool"));
		item->setData(kExtNameRole, name);
		m_listWidget->addItem(item);
	}

	const QStringList clientNames = ClientExtension::installedNames();
	const QStringList clientLoaded = ClientExtension::loadedNames();
	for (const QString& name : clientNames) {
		// 后缀标出种类；未装载的再加一段
		QString label = name + QStringLiteral("  [") + qtTrId("ext_kind_client");
		if (!clientLoaded.contains(name))
			label += QStringLiteral("·") + qtTrId("ext_client_not_loaded");
		label += QStringLiteral("]");

		auto* item = new QListWidgetItem(label);
		item->setData(kExtKindRole, QStringLiteral("client"));
		item->setData(kExtNameRole, name);
		m_listWidget->addItem(item);
	}

	const int total = toolNames.size() + clientNames.size();
	if (total == 0) {
		setStatus(qtTrId("ext_empty_hint"));
	}
	else {
		setStatus(qtTrId("ext_installed_count_fmt").arg(total));
	}

	m_removeButton->setEnabled(false);
}

void ExtensionManagerPopup::installExtension()
{
	if (m_installTask.isRunning())
		return;

	const QString extPath = QFileDialog::getOpenFileName(this, qtTrId("ext_choose_file_title"),
		QString(), QStringLiteral("DSH Extension (*.ext)"));

	if (extPath.isEmpty())
		return;
	qInfo().noquote() << QStringLiteral("[ExtensionManager] installExtension start: %1").arg(extPath);

	if (m_installButton)
		m_installButton->setEnabled(false);
	setStatus(qtTrId("ext_installing"));

	if (!m_installTask.start(extPath, m_registry.serverProfilePath())) {
		if (m_installButton)
			m_installButton->setEnabled(true);
		return;
	}

	if (m_installTimer)
		m_installTimer->start();
	qInfo().noquote() << QStringLiteral("[ExtensionManager] async install started, timer running");
}

void ExtensionManagerPopup::pollInstall()
{
	if (!m_installTask.tryFinish())
		return;
	qInfo().noquote() << QStringLiteral("[ExtensionManager] pollInstall: future ready");

	if (m_installTimer)
		m_installTimer->stop();
	if (m_installButton)
		m_installButton->setEnabled(true);

	if (!m_installTask.succeeded()) {
		setStatus(qtTrId("ext_install_failed_fmt").arg(m_installTask.errorString()));
		return;
	}
	qInfo().noquote() << QStringLiteral("[ExtensionManager] loadAndInstall returned ok=true");

	const ExtensionLoader::LoadedExtension& ext = m_installTask.extension();

	// 客户端扩展不写 extensions.json、不重启服务端；装载必须留在 GUI 线程
	if (ext.isClientExtension) {
		QString loadError;
		if (ClientExtension::loadOne(ext.dllPath, ext.pluginName, &loadError)) {
			setStatus(qtTrId("ext_install_success_fmt").arg(ext.pluginName));
			qInfo().noquote() << QStringLiteral("[ExtensionManager] client extension loaded:")
				<< ext.pluginName;
		}
		else {
			setStatus(qtTrId("ext_install_failed_fmt").arg(loadError));
			qWarning().noquote() << QStringLiteral("[ExtensionManager] client extension load failed:")
				<< ext.pluginName << loadError;
		}

		// 立刻重建列表：客户端扩展的"已安装"判据是目录，刚变过
		populateList();
		return;
	}

	m_registry.registerInstalled(ext.pluginName);
	qInfo().noquote() << QStringLiteral("[ExtensionManager] installed registry updated");

	populateList();
	setStatus(qtTrId("ext_install_success_fmt").arg(ext.pluginName));
	qInfo().noquote() << QStringLiteral("[ExtensionManager] list populated, emitting extensionInstalled");

	// 用扩展目录里持久化的 json5/dll，不依赖临时解压目录
	if (ext.pluginName.isEmpty())
		return;

	emit extensionInstalled(m_registry.extensionJsonPath(ext.pluginName),
		m_registry.extensionDllPath(ext.pluginName));
	qInfo().noquote() <<
		QStringLiteral("[ExtensionManager] extensionInstalled emitted, emitting serverRestartRequested");
	emit serverRestartRequested();
}

void ExtensionManagerPopup::removeSelected()
{
	QListWidgetItem* item = m_listWidget->currentItem();
	if (!item)
		return;

	// 真名取自 UserRole，显示文本带后缀不能当名字
	const QString name = item->data(kExtNameRole).toString();
	const bool isClientExtension = item->data(kExtKindRole).toString() == QLatin1String("client");

	if (QMessageBox::question(this, qtTrId("ext_remove_confirm_title"), qtTrId("ext_remove_confirm_fmt").arg(name),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
		return;
	}

	if (isClientExtension) {
		// 已装载插件进程内卸载不了，要重启才彻底消失
		QString removeError;
		const bool removed = ClientExtension::remove(name, &removeError);
		populateList();
		setStatus(removed ? qtTrId("ext_removed_fmt").arg(name)
			: qtTrId("ext_partial_delete_failed_fmt").arg(removeError));
		return;
	}

	// 先让 DSH Hub 卸载 DLL，否则 main.dll 被占用删不掉
	emit extensionRemoving(name);

	// 目录删没删成功都要清配置残留，否则 patch 引用不存在的包
	QString dirError;
	const bool dirRemoved = m_registry.removeExtensionDirectory(name, &dirError);
	const bool patchRemoved = ExtensionRegistry::removePatchEntry(m_registry.serverProfilePath(), name);
	m_registry.unregisterInstalled(name);

	populateList();

	if (dirRemoved && patchRemoved) {
		setStatus(qtTrId("ext_removed_fmt").arg(name));
	}
	else if (!dirRemoved) {
		setStatus(qtTrId("ext_partial_delete_failed_fmt").arg(dirError));
	}
	else {
		setStatus(qtTrId("ext_patch_update_failed_fmt").arg(name));
	}

	emit serverRestartRequested();
}

void ExtensionManagerPopup::cleanupResiduals()
{
	qInfo().noquote() << "[ExtensionManager] cleanupResiduals called";

	const ExtensionRegistry::CleanupResult result = m_registry.cleanupResiduals();
	if (!result.patchReadable) {
		setStatus(qtTrId("ext_patch_read_failed"));
		return;
	}

	populateList();
	setStatus(result.removed.isEmpty() ? qtTrId("ext_no_residue")
		: qtTrId("ext_residue_cleaned_fmt").arg(result.removed.join(QLatin1String(", "))));

	if (!result.removed.isEmpty())
		emit serverRestartRequested();
}

void ExtensionManagerPopup::refresh()
{
	populateList();
}
