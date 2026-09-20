#include "ui/ExtensionManagerPopup.h"

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
	// 列表条目挂在 UserRole 上的元数据。显示文本带种类后缀，移除时必须用**真名**
	// —— 以前直接拿 item->text() 当名字，加了后缀就会去删一个不存在的扩展。
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

	// 两种扩展的**数据源不同**，所以这里合并两个来源：
	//   工具扩展   —— extensions.json（服务端扩展清单）
	//   客户端扩展 —— <exe>/clientExtensions/ 目录。那是"已安装"的真相：重启后仍在，
	//                 而且**装载失败也在**（这一点很关键，否则失败时界面全空、
	//                 用户无从判断是没装上还是没起来）。
	// 两者不是一套东西，见 ExtensionSystem/ClientExtension.h 开头的对照表。
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
		// 后缀标出种类；没装载上的再加一段 —— "装上了却没起来"是这里最要紧的信息。
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

	// ---- 客户端扩展（ClientExtension）：收尾与工具扩展完全不同 ----
	// 它不是服务端的工具扩展：不写 extensions.json、不改 cordis.patch.yml、
	// 不重启服务端，也不出现在这个列表里（列表画的是 extensions.json）。
	// 这里只做一件事：把「名字 + 已落地的 dll 路径」交给 DSHHub 去装载 ——
	// 装载必须在 GUI 线程（ExtensionInstallTask 跑在后台线程，不能在那边装）。
	if (ext.isClientExtension) {
		// 客户端扩展：装载**就在这里做** —— pollInstall 本来跑在 GUI 线程，
		// 而装载必须在 GUI 线程（插件要查注册表、往宿主布局挂控件）。
		//
		// 为什么不像原先那样交给 DSHHub 再发个信号：那样只能先报"安装成功"、
		// 装载失败却只写日志，用户看到的是"装好了但没反应"（踩过：
		// Debug 宿主 + Release 插件被 Qt 拦掉时就是这样）。
		// 在这里装载就能把**真实结果**直接写进状态栏。
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
		return;
	}

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

	// 真名取自条目的 UserRole —— 显示文本带了种类后缀，不能拿来当名字用
	const QString name = item->data(kExtNameRole).toString();
	const bool isClientExtension =
		item->data(kExtKindRole).toString() == QLatin1String("client");

	if (QMessageBox::question(
		this,
		qtTrId("ext_remove_confirm_title"),
		qtTrId("ext_remove_confirm_fmt").arg(name),
		QMessageBox::Yes | QMessageBox::No,
		QMessageBox::No) != QMessageBox::Yes) {
		return;
	}

	if (isClientExtension) {
		// 客户端扩展：只删它自己的目录 —— 不碰 extensions.json / cordis.patch.yml，
		// 也不重启服务端（它本来就不是服务端的东西）。
		// ⚠️ 已装载的插件在进程内卸载不了（控件还挂在宿主窗口里），要重启才彻底消失。
		QString removeError;
		const bool removed = ClientExtension::remove(name, &removeError);
		populateList();
		setStatus(removed
			? qtTrId("ext_removed_fmt").arg(name)
			: qtTrId("ext_partial_delete_failed_fmt").arg(removeError));
		return;
	}

	// ---- 工具扩展：以下原样 ----
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