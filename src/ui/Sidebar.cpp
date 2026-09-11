#include "Sidebar.h"
#include "ThemeManager.h"

#include "DshApiClient.h"
#include "SessionService.h"

#include <QContextMenuEvent>
#include <QDialog>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QSize>
#include <QSizePolicy>
#include <QVBoxLayout>

// ------------------------------------------------------------------
// SidebarLogo
// ------------------------------------------------------------------

SidebarLogo::SidebarLogo(QWidget* parent)
	: QLabel(parent)
{
	setObjectName(QStringLiteral("sidebarLogo"));
	setAlignment(Qt::AlignCenter);
	setAttribute(Qt::WA_TranslucentBackground);

	const QString logoResource = Theme::isDark()
		? QStringLiteral(":/DSHHub/DSH-Hub-Logo-Tiny-Dark@2x.png")
		: QStringLiteral(":/DSHHub/DSH-Hub-Logo-Tiny@2x.png");
	QPixmap logoPix(logoResource);
	if (!logoPix.isNull()) {
		logoPix.setDevicePixelRatio(2.0);
		setPixmap(logoPix);
		setFixedHeight(logoPix.height() / logoPix.devicePixelRatio());
	}
	else {
		setText(QStringLiteral("DSH Hub"));
		setFixedHeight(80);
	}
}

// ------------------------------------------------------------------
// NewWorkspaceButton
// ------------------------------------------------------------------

NewWorkspaceButton::NewWorkspaceButton(QWidget* parent)
	: QPushButton(QStringLiteral("新建工作区"), parent)
{
	setObjectName(QStringLiteral("newWorkspaceButton"));
	setCursor(Qt::PointingHandCursor);
}

// ------------------------------------------------------------------
// ClearSessionButton
// ------------------------------------------------------------------

ClearSessionButton::ClearSessionButton(QWidget* parent)
	: QPushButton(QStringLiteral("清空会话"), parent)
{
	setObjectName(QStringLiteral("clearSessionButton"));
	setCursor(Qt::PointingHandCursor);
}

// ------------------------------------------------------------------
// SidebarSettingsButton
// ------------------------------------------------------------------

SidebarSettingsButton::SidebarSettingsButton(QWidget* parent)
	: QPushButton(parent)
{
	setObjectName(QStringLiteral("sidebarSettingsButton"));
	setFixedSize(32, 32);
	setCursor(Qt::PointingHandCursor);
	setToolTip(QStringLiteral("设置"));
	setIcon(QIcon(QStringLiteral(":/DSHHub/Setting-Icon.png")));
	setIconSize(QSize(20, 20));
}

// ------------------------------------------------------------------
// SidebarPluginsButton
// ------------------------------------------------------------------

SidebarPluginsButton::SidebarPluginsButton(QWidget* parent)
	: QPushButton(parent)
{
	setObjectName(QStringLiteral("sidebarPluginsButton"));
	setFixedSize(32, 32);
	setCursor(Qt::PointingHandCursor);
	setToolTip(QStringLiteral("插件"));
	setIcon(QIcon(QStringLiteral(":/DSHHub/Plugin-Icon.png")));
	setIconSize(QSize(20, 20));
}

// ------------------------------------------------------------------
// SidebarThemeButton
// ------------------------------------------------------------------

SidebarThemeButton::SidebarThemeButton(QWidget* parent)
	: QPushButton(parent)
{
	setObjectName(QStringLiteral("sidebarThemeButton"));
	setFixedSize(32, 32);
	setCursor(Qt::PointingHandCursor);
	setToolTip(QStringLiteral("切换主题"));
	setIcon(QIcon(QStringLiteral(":/DSHHub/Theme-Icon.png")));
	setIconSize(QSize(20, 20));
}

// ------------------------------------------------------------------
// SidebarExtensionButton
// ------------------------------------------------------------------

SidebarExtensionButton::SidebarExtensionButton(QWidget* parent)
	: QPushButton(parent)
{
	setObjectName(QStringLiteral("sidebarExtensionButton"));
	setFixedSize(32, 32);
	setCursor(Qt::PointingHandCursor);
	setToolTip(QStringLiteral("扩展管理"));
	setIcon(QIcon(QStringLiteral(":/DSHHub/Extension-Icon.png")));
	setIconSize(QSize(20, 20));
}

// ------------------------------------------------------------------
// SessionButton
// ------------------------------------------------------------------

SessionButton::SessionButton(const QString& sessionId,
	const QString& title,
	QWidget* parent)
	: QPushButton(parent)
	, m_sessionId(sessionId)
	, m_fullTitle(title.isEmpty() ? sessionId : title)
{
	setObjectName(QStringLiteral("sessionButton"));
	setCheckable(true);
	// 不再依赖 autoExclusive 做跨工作区互斥，统一由 WorkspaceList::setCurrentSession 管理
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setMinimumHeight(32);
	setCursor(Qt::PointingHandCursor);

	connect(this, &QPushButton::clicked, this, &SessionButton::handleClicked);

	updateElidedText();
}

QString SessionButton::sessionId() const
{
	return m_sessionId;
}

QString SessionButton::fullTitle() const
{
	return m_fullTitle;
}

void SessionButton::setSessionTitle(const QString& title)
{
	m_fullTitle = title.isEmpty() ? m_sessionId : title;
	updateElidedText();
}

void SessionButton::setSelected(bool selected)
{
	setChecked(selected);
}

void SessionButton::resizeEvent(QResizeEvent* event)
{
	QPushButton::resizeEvent(event);
	updateElidedText();
}

void SessionButton::handleClicked()
{
	emit sessionClicked(m_sessionId);
}

void SessionButton::contextMenuEvent(QContextMenuEvent* event)
{
	// 不用 QMenu：Windows 的 QMenu 原生弹窗即使设置 WA_TranslucentBackground，
	// 在某些环境下仍会在圆角外绘制黑色矩形。这里改用和 PopupWindow 相同的
	// 无边框透明 QDialog + QPushButton 实现，圆角外可以真正透明。
	QDialog menu(nullptr, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
	menu.setAttribute(Qt::WA_TranslucentBackground);
	menu.setAttribute(Qt::WA_StyledBackground, true);

	auto* deleteAction = new QPushButton(QStringLiteral("删除会话"), &menu);
	deleteAction->setObjectName(QStringLiteral("sessionContextDeleteAction"));
	deleteAction->setCursor(Qt::PointingHandCursor);
	deleteAction->setStyleSheet(
		QStringLiteral("QPushButton#sessionContextDeleteAction {")
		+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";")
		+ QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";")
		+ QStringLiteral("  border-radius: 10px;")
		+ QStringLiteral("  padding: 8px 20px;")
		+ QStringLiteral("  font-size: 13px;")
		+ QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";")
		+ QStringLiteral("}")
		+ QStringLiteral("QPushButton#sessionContextDeleteAction:hover {")
		+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("dangerBg")) + QStringLiteral(";")
		+ QStringLiteral("  color: ") + Theme::color(QStringLiteral("danger")) + QStringLiteral(";")
		+ QStringLiteral("}"));

	auto* layout = new QVBoxLayout(&menu);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(deleteAction);

	menu.adjustSize();
	menu.move(event->globalPos());

	connect(deleteAction, &QPushButton::clicked, this, [this, &menu]() {
		emit deleteRequested(m_sessionId);
		menu.accept();
		});

	menu.exec();
}

void SessionButton::updateElidedText()
{
	const int availableWidth = width() - 20;
	if (availableWidth <= 0) {
		setText(m_fullTitle);
		return;
	}

	const QFontMetrics fm(font());
	setText(fm.elidedText(m_fullTitle, Qt::ElideRight, availableWidth));
}

// ------------------------------------------------------------------
// WorkspaceButton
// ------------------------------------------------------------------

WorkspaceButton::WorkspaceButton(const QString& title, QWidget* parent)
	: QPushButton(parent)
	, m_title(title)
{
	setObjectName(QStringLiteral("workspaceButton"));
	setCursor(Qt::PointingHandCursor);
	setMouseTracking(true);
	setCheckable(true);
	setChecked(true);

	setExpanded(true);
}

void WorkspaceButton::setExpanded(bool expanded)
{
	setText((expanded ? QStringLiteral("▾ ") : QStringLiteral("▸ ")) + m_title);
	update();
}

void WorkspaceButton::enterEvent(QEnterEvent* event)
{
	update();
	QPushButton::enterEvent(event);
}

void WorkspaceButton::leaveEvent(QEvent* event)
{
	m_plusHovered = false;
	update();
	QPushButton::leaveEvent(event);
}

void WorkspaceButton::mouseMoveEvent(QMouseEvent* event)
{
	const bool hover = rect().contains(event->pos()) && event->pos().x() >= width() - 30;
	if (hover != m_plusHovered) {
		m_plusHovered = hover;
		update();
	}
	QPushButton::mouseMoveEvent(event);
}

void WorkspaceButton::mouseReleaseEvent(QMouseEvent* event)
{
	// 点击右侧加号区域时只触发新建会话，不折叠/展开工作区
	if (event->pos().x() >= width() - 30) {
		emit addSessionRequested();
		return;
	}
	QPushButton::mouseReleaseEvent(event);
}

void WorkspaceButton::paintEvent(QPaintEvent* event)
{
	QPushButton::paintEvent(event);

	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);

	const int cx = width() - 16;
	const int cy = height() / 2;
	const int radius = 8;

	// 只有鼠标悬停在加号区域时，才显示浅蓝色圆形背景
	if (m_plusHovered) {
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(224, 231, 255));
		painter.drawEllipse(QPointF(cx, cy), radius, radius);
	}

	QPen pen(QColor(76, 139, 245), 2, Qt::SolidLine, Qt::RoundCap);
	painter.setPen(pen);

	const int half = 5;
	painter.drawLine(cx - half, cy, cx + half, cy);
	painter.drawLine(cx, cy - half, cx, cy + half);
}

// ------------------------------------------------------------------
// WorkspaceList
// ------------------------------------------------------------------

WorkspaceList::WorkspaceList(QWidget* parent)
	: QWidget(parent)
{
	setObjectName(QStringLiteral("workspaceList"));
	setAttribute(Qt::WA_StyledBackground, true);

	m_layout = new QVBoxLayout(this);
	m_layout->setContentsMargins(0, 0, 0, 0);
	m_layout->setSpacing(2);
	m_layout->addStretch(1);
}

SessionCatalog& WorkspaceList::catalog()
{
	return m_catalog;
}

const SessionCatalog& WorkspaceList::catalog() const
{
	return m_catalog;
}

void WorkspaceList::rebuildFromCatalog()
{
	clearWorkspaceGroups();

	// 先建工作区分组（标题为空时退回用 id 显示），再往里面挂会话按钮
	for (const WorkspaceRecord& workspace : m_catalog.workspaces())
		createWorkspaceGroup(workspace.workspaceId, workspace.title);

	for (const SessionRecord& session : m_catalog.visibleSessions())
		addSessionButton(session.sessionId, session.title);
}

WorkspaceList::WorkspaceGroup* WorkspaceList::createWorkspaceGroup(const QString& workspaceId, const QString& title)
{
	auto* group = new WorkspaceGroup;
	group->workspaceId = workspaceId;
	group->container = new QWidget(this);
	group->container->setObjectName(QStringLiteral("workspaceGroupContainer"));
	group->container->setAttribute(Qt::WA_StyledBackground, true);
	group->layout = new QVBoxLayout(group->container);
	group->layout->setContentsMargins(0, 0, 0, 0);
	group->layout->setSpacing(2);
	group->header = new WorkspaceButton(title, group->container);
	group->layout->addWidget(group->header);

	connect(group->header, &QPushButton::toggled, this, [this, group](bool checked) {
		group->expanded = checked;
		group->header->setExpanded(checked);
		for (SessionButton* button : group->buttons)
			button->setVisible(checked);
		});
	connect(group->header, &WorkspaceButton::addSessionRequested, this, [this, group]() {
		emit createSessionInWorkspaceRequested(group->workspaceId);
		});

	m_layout->insertWidget(m_layout->count() - 1, group->container);
	m_workspaceGroups.push_back(group);

	if (workspaceId.isEmpty())
		m_defaultGroup = group;

	return group;
}

WorkspaceList::WorkspaceGroup* WorkspaceList::defaultGroup()
{
	if (!m_defaultGroup)
		m_defaultGroup = createWorkspaceGroup(QString(), QStringLiteral("未分组"));
	return m_defaultGroup;
}

WorkspaceList::WorkspaceGroup* WorkspaceList::groupFor(const QString& workspaceId)
{
	for (WorkspaceGroup* group : m_workspaceGroups) {
		if (group->workspaceId == workspaceId)
			return group;
	}
	// 找不到归属工作区（含未分组）时统一落到“未分组”
	return defaultGroup();
}

void WorkspaceList::clearWorkspaceGroups()
{
	for (WorkspaceGroup* group : m_workspaceGroups) {
		if (group->container)
			delete group->container;
		delete group;
	}
	m_workspaceGroups.clear();
	m_buttons.clear();
	m_defaultGroup = nullptr;
}

void WorkspaceList::addSessionButton(const QString& sessionId, const QString& title)
{
	// 防止同一个会话被重复添加，避免 setCurrentSession 选中多个同 sessionId 的按钮
	for (SessionButton* button : m_buttons) {
		if (button->sessionId() == sessionId) {
			button->setSessionTitle(title);
			return;
		}
	}

	WorkspaceGroup* group = groupFor(m_catalog.workspaceFor(sessionId));

	auto* button = new SessionButton(sessionId, title, group->container);
	connect(button, &SessionButton::sessionClicked, this, [this, sessionId]() {
		setCurrentSession(sessionId);
		emit sessionSelected(sessionId);
		});
	connect(button, &SessionButton::deleteRequested, this, [this](const QString& sid) {
		emit deleteSessionRequested(sid);
		});

	group->layout->addWidget(button);
	group->buttons.push_back(button);
	m_buttons.push_back(button);

	if (!group->expanded)
		button->setVisible(false);
}

void WorkspaceList::addSession(const QString& sessionId, const QString& title)
{
	// 已归档/已删除的会话由 catalog 过滤，不再显示在侧边栏
	if (!m_catalog.addSession(sessionId, title))
		return;

	addSessionButton(sessionId, title);
}

void WorkspaceList::addSessionToWorkspace(const QString& sessionId, const QString& title, const QString& workspaceId)
{
	if (!m_catalog.addSession(sessionId, title, workspaceId))
		return;

	addSessionButton(sessionId, title);
}

void WorkspaceList::clearSessions()
{
	m_catalog.clear();
	clearWorkspaceGroups();
}

void WorkspaceList::setCurrentSession(const QString& sessionId)
{
	for (SessionButton* button : m_buttons) {
		const bool selected = button->sessionId() == sessionId;
		if (button->isChecked() != selected)
			button->setSelected(selected);
	}
}

void WorkspaceList::updateSessionTitle(const QString& sessionId, const QString& title)
{
	if (!m_catalog.updateTitle(sessionId, title))
		return;

	for (SessionButton* button : m_buttons) {
		if (button->sessionId() == sessionId) {
			button->setSessionTitle(title);
			break;
		}
	}
}

QString WorkspaceList::titleForSession(const QString& sessionId) const
{
	return m_catalog.titleFor(sessionId);
}

void WorkspaceList::refreshTitles(DshApiClient* api)
{
	SessionService::refreshTitles(api, [this](const QString& sessionId, const QString& title) {
		updateSessionTitle(sessionId, title);
		});
}

// ------------------------------------------------------------------
// Sidebar
// ------------------------------------------------------------------

Sidebar::Sidebar(QWidget* parent)
	: QWidget(parent)
{
	setObjectName(QStringLiteral("sidebar"));
	setAttribute(Qt::WA_StyledBackground, true);

	m_logo = new SidebarLogo(this);
	m_clearButton = new ClearSessionButton(this);
	m_newWorkspaceButton = new NewWorkspaceButton(this);
	m_workspaceList = new WorkspaceList(this);
	m_settingsButton = new SidebarSettingsButton(this);
	m_pluginsButton = new SidebarPluginsButton(this);
	m_themeButton = new SidebarThemeButton(this);
	m_extensionButton = new SidebarExtensionButton(this);

	m_layout = new QVBoxLayout(this);
	m_layout->setContentsMargins(10, 10, 10, 10);
	m_layout->setSpacing(4);
	m_layout->addWidget(m_logo, 0, Qt::AlignHCenter);
	m_layout->addWidget(m_clearButton);
	m_layout->addWidget(m_newWorkspaceButton);
	m_layout->addWidget(m_workspaceList, 1);

	auto* bottomRow = new QHBoxLayout;
	bottomRow->setContentsMargins(0, 0, 0, 0);
	bottomRow->setSpacing(0);
	bottomRow->addWidget(m_settingsButton);
	bottomRow->addWidget(m_pluginsButton);
	bottomRow->addWidget(m_themeButton);
	bottomRow->addWidget(m_extensionButton);
	bottomRow->addSpacing(15);
	bottomRow->addStretch(1);

	m_layout->addLayout(bottomRow);

	connect(m_clearButton, &QPushButton::clicked,
		this, &Sidebar::clearRequested);
	connect(m_newWorkspaceButton, &QPushButton::clicked,
		this, &Sidebar::newWorkspaceRequested);
	connect(m_settingsButton, &QPushButton::clicked,
		this, &Sidebar::settingsRequested);
	connect(m_pluginsButton, &QPushButton::clicked,
		this, &Sidebar::pluginsRequested);
	connect(m_themeButton, &QPushButton::clicked,
		this, &Sidebar::themeToggleRequested);
	connect(m_extensionButton, &QPushButton::clicked,
		this, &Sidebar::extensionsRequested);

	connect(m_workspaceList, &WorkspaceList::sessionSelected,
		this, &Sidebar::sessionSelected);
	connect(m_workspaceList, &WorkspaceList::createSessionInWorkspaceRequested,
		this, &Sidebar::createSessionInWorkspaceRequested);
	connect(m_workspaceList, &WorkspaceList::deleteSessionRequested,
		this, &Sidebar::deleteSessionRequested);
}

WorkspaceList* Sidebar::workspaceList() const
{
	return m_workspaceList;
}

void Sidebar::addCreatedSession(const QString& sessionId, const QString& workspaceId)
{
	if (sessionId.isEmpty() || !m_workspaceList)
		return;

	if (workspaceId.isEmpty())
		m_workspaceList->addSession(sessionId, QStringLiteral("未命名会话"));
	else
		m_workspaceList->addSessionToWorkspace(sessionId, QStringLiteral("未命名会话"), workspaceId);

	m_workspaceList->setCurrentSession(sessionId);
}

void Sidebar::refreshSessions(DshApiClient* api, SessionPrefetcher* prefetcher)
{
	if (!m_workspaceList)
		return;

	SessionService::refreshSessions(api, prefetcher, &m_workspaceList->catalog(),
		[this](const QString& autoSelectSessionId) {
			// 数据已写入 catalog，这里只重建视图并转发结果
			m_workspaceList->rebuildFromCatalog();

			// auto select the first available non-running session
			if (!autoSelectSessionId.isEmpty())
				emit initialSessionReady(autoSelectSessionId, m_workspaceList->titleForSession(autoSelectSessionId));
			else
				emit noSessionAvailable();
		},
		[this](const DshApiClient::RpcError& error) {
			m_workspaceList->rebuildFromCatalog();
			emit sessionListError(error.code, error.message);
		});
}

void Sidebar::createSession(DshApiClient* api, const QString& workspaceId)
{
	SessionService::createSession(api, workspaceId,
		[this, workspaceId](const QString& sessionId) {
			addCreatedSession(sessionId, workspaceId);
			emit sessionCreated(sessionId, workspaceId);
		},
		[this](const DshApiClient::RpcError& error) {
			emit sessionCreateError(error.code, error.message);
		});
}

void Sidebar::clearAllSessions(
	const QString& dshHome,
	const std::function<void()>& onCleared,
	const std::function<void()>& onCreateNew)
{
	// 文件/目录清理由 common 层负责
	SessionService::clearAllSessionData(dshHome);

	// 与当前加载的会话相关的清理由 WorkspaceList（catalog）负责
	if (m_workspaceList)
		m_workspaceList->clearSessions();

	if (onCleared)
		onCleared();

	if (onCreateNew)
		onCreateNew();
}