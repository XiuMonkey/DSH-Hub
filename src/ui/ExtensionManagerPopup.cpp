#include "ExtensionManagerPopup.h"

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

	// 顶部按钮栏
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

	// 扩展列表
	m_listWidget = new QListWidget(content);
	m_listWidget->setObjectName(QStringLiteral("extPopupList"));
	m_listWidget->setSelectionMode(QAbstractItemView::SingleSelection);

	rootLayout->addWidget(m_listWidget, 1);

	// 状态栏
	m_statusLabel = new QLabel(content);
	m_statusLabel->setObjectName(QStringLiteral("extPopupStatus"));
	attachStatusLabel(m_statusLabel); // 宽度布局生效后重排状态文本

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
		m_installTask.waitForFinished();
		});

	refresh();
}

void ExtensionManagerPopup::populateList()
{
	m_listWidget->clear();

	const QStringList names = m_registry.installedExtensions();
	for (const QString& name : names)
		m_listWidget->addItem(name);

	if (names.isEmpty()) {
		setStatus(qtTrId("ext_empty_hint"));
	}
	else {
		setStatus(qtTrId("ext_installed_count_fmt").arg(names.size()));
	}

	m_removeButton->setEnabled(false);
}

void ExtensionManagerPopup::installExtension()
{
	if (m_installTask.isRunning())
		return;

	const QString extPath = QFileDialog::getOpenFileName(
		this,
		qtTrId("ext_choose_file_title"),
		QString(),
		QStringLiteral("DSH Extension (*.ext)"));

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
	m_registry.registerInstalled(ext.pluginName);
	qInfo().noquote() << QStringLiteral("[ExtensionManager] installed registry updated");

	populateList();
	setStatus(qtTrId("ext_install_success_fmt").arg(ext.pluginName));
	qInfo().noquote() << QStringLiteral("[ExtensionManager] list populated, emitting extensionInstalled");

	// 使用持久化到扩展目录的 regulation.json5 / main.dll，
	// 避免依赖临时解压目录，重启后也能从同一位置加载。
	if (ext.pluginName.isEmpty())
		return;

	emit extensionInstalled(
		m_registry.extensionJsonPath(ext.pluginName),
		m_registry.extensionDllPath(ext.pluginName));
	qInfo().noquote() << QStringLiteral("[ExtensionManager] extensionInstalled emitted, emitting serverRestartRequested");
	emit serverRestartRequested();
}

void ExtensionManagerPopup::removeSelected()
{
	QListWidgetItem* item = m_listWidget->currentItem();
	if (!item)
		return;

	const QString name = item->text();

	if (QMessageBox::question(
		this,
		qtTrId("ext_remove_confirm_title"),
		qtTrId("ext_remove_confirm_fmt").arg(name),
		QMessageBox::Yes | QMessageBox::No,
		QMessageBox::No) != QMessageBox::Yes) {
		return;
	}

	// 通知 DSH Hub 先卸载当前 DLL，避免 main.dll 被占用导致无法删除
	emit extensionRemoving(name);

	// 无论目录是否删除成功，都要清理配置残留，避免 cordis.patch.yml 引用不存在的包
	QString dirError;
	const bool dirRemoved = m_registry.removeExtensionDirectory(name, &dirError);
	const bool patchRemoved = ExtensionRegistry::removePatchEntry(m_registry.serverProfilePath(), name);
	m_registry.unregisterInstalled(name);

	populateList();

	if (dirRemoved && patchRemoved) {
		setStatus(qtTrId("ext_removed_fmt").arg(name));
	}
	else if (!dirRemoved) {
		setStatus(
			qtTrId("ext_partial_delete_failed_fmt").arg(dirError));
	}
	else {
		setStatus(
			qtTrId("ext_patch_update_failed_fmt").arg(name));
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
	setStatus(result.removed.isEmpty()
		? qtTrId("ext_no_residue")
		: qtTrId("ext_residue_cleaned_fmt").arg(result.removed.join(QLatin1String(", "))));

	if (!result.removed.isEmpty())
		emit serverRestartRequested();
}

void ExtensionManagerPopup::refresh()
{
	populateList();
}