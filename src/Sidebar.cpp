#include "Sidebar.h"
#include "ThemeManager.h"

#include "DshApiClient.h"

#include <QDir>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
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

SidebarLogo::SidebarLogo(QWidget *parent)
    : QLabel(parent)
{
    setObjectName(QStringLiteral("sidebarLogo"));
    
setAlignment(Qt::AlignCenter);
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet(QStringLiteral("background: transparent;"));

    QPixmap logoPix(QStringLiteral(":/DSHHub/DSH-Hub-Logo-Tiny@2x.png"));
    if (!logoPix.isNull()) {
        logoPix.setDevicePixelRatio(2.0);
        setPixmap(logoPix);
        setFixedHeight(logoPix.height() / logoPix.devicePixelRatio());
    } else {
        setText(QStringLiteral("DSH Hub"));
        setFixedHeight(80);
    }
}

// ------------------------------------------------------------------
// NewWorkspaceButton
// ------------------------------------------------------------------

NewWorkspaceButton::NewWorkspaceButton(QWidget *parent)
    : QPushButton(QStringLiteral("新建工作区"), parent)
{
    setObjectName(QStringLiteral("newWorkspaceButton"));
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(QStringLiteral("QPushButton#newWorkspaceButton {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";") + QStringLiteral("  color: white;") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("  padding: 8px 12px;") + QStringLiteral("  font-size: 14px;") + QStringLiteral("}") + QStringLiteral("QPushButton#newWorkspaceButton:hover {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("accentHover")) + QStringLiteral(";") + QStringLiteral("}"));
}

// ------------------------------------------------------------------
// ClearSessionButton
// ------------------------------------------------------------------

ClearSessionButton::ClearSessionButton(QWidget *parent)
    : QPushButton(QStringLiteral("清空会话"), parent)
{
    setObjectName(QStringLiteral("clearSessionButton"));
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(QStringLiteral("QPushButton#clearSessionButton {") + QStringLiteral("  background: ") + QStringLiteral("#E5484D") + QStringLiteral(";") + QStringLiteral("  color: white;") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("  padding: 8px 12px;") + QStringLiteral("  font-size: 14px;") + QStringLiteral("}") + QStringLiteral("QPushButton#clearSessionButton:hover {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("danger")) + QStringLiteral(";") + QStringLiteral("}"));
}

// ------------------------------------------------------------------
// SidebarSettingsButton
// ------------------------------------------------------------------

SidebarSettingsButton::SidebarSettingsButton(QWidget *parent)
    : QPushButton(parent)
{
    setObjectName(QStringLiteral("sidebarSettingsButton"));
    setFixedSize(32, 32);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("设置"));
    setIcon(QIcon(QStringLiteral(":/DSHHub/Setting-Icon.png")));
    setIconSize(QSize(20, 20));
    setStyleSheet(QStringLiteral("QPushButton {") + QStringLiteral("  background: transparent;") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("}") + QStringLiteral("QPushButton:hover {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg")) + QStringLiteral(";") + QStringLiteral("}"));
}

// ------------------------------------------------------------------
// SidebarPluginsButton
// ------------------------------------------------------------------

SidebarPluginsButton::SidebarPluginsButton(QWidget *parent)
    : QPushButton(parent)
{
    setObjectName(QStringLiteral("sidebarPluginsButton"));
    setFixedSize(32, 32);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("插件"));
    setIcon(QIcon(QStringLiteral(":/DSHHub/Plugin-Icon.png")));
    setIconSize(QSize(20, 20));
    setStyleSheet(QStringLiteral("QPushButton#sidebarPluginsButton {") + QStringLiteral("  background: transparent;") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("}") + QStringLiteral("QPushButton#sidebarPluginsButton:hover {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg")) + QStringLiteral(";") + QStringLiteral("}"));
}

// ------------------------------------------------------------------
// SidebarThemeButton
// ------------------------------------------------------------------

SidebarThemeButton::SidebarThemeButton(QWidget *parent)
    : QPushButton(parent)
{
    setObjectName(QStringLiteral("sidebarThemeButton"));
    setFixedSize(32, 32);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("切换主题"));
    setIcon(QIcon(QStringLiteral(":/DSHHub/Theme-Icon.png")));
    setIconSize(QSize(20, 20));
    setStyleSheet(QStringLiteral("QPushButton#sidebarThemeButton {") + QStringLiteral("  background: transparent;") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("}") + QStringLiteral("QPushButton#sidebarThemeButton:hover {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg")) + QStringLiteral(";") + QStringLiteral("}"));
}


    

// ------------------------------------------------------------------
// SessionButton
// ------------------------------------------------------------------

SessionButton::SessionButton(const QString &sessionId,
                             const QString &title,
                             QWidget *parent)
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

void SessionButton::setSessionTitle(const QString &title)
{
    m_fullTitle = title.isEmpty() ? m_sessionId : title;
    updateElidedText();
}

void SessionButton::setSelected(bool selected)
{
    setChecked(selected);
}

void SessionButton::resizeEvent(QResizeEvent *event)
{
    QPushButton::resizeEvent(event);
    updateElidedText();
}

void SessionButton::handleClicked()
{
    emit sessionClicked(m_sessionId);
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

WorkspaceButton::WorkspaceButton(const QString &title, QWidget *parent)
    : QPushButton(parent)
    , m_title(title)
{
    setObjectName(QStringLiteral("workspaceButton"));
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    setCheckable(true);
    setChecked(true);
    setStyleSheet(QStringLiteral("QPushButton#workspaceButton {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg")) + QStringLiteral(";") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("  padding: 6px 26px 6px 10px;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("  font-size: 13px;") + QStringLiteral("  font-weight: 600;") + QStringLiteral("  text-align: left;") + QStringLiteral("}") + QStringLiteral("QPushButton#workspaceButton:hover {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("activeBg")) + QStringLiteral(";") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("}"));

    setExpanded(true);
}

void WorkspaceButton::setExpanded(bool expanded)
{
    m_expanded = expanded;
    setText((expanded ? QStringLiteral("▾ ") : QStringLiteral("▸ ")) + m_title);
    update();
}

void WorkspaceButton::enterEvent(QEnterEvent *event)
{
    m_showPlus = true;
    update();
    QPushButton::enterEvent(event);
}

void WorkspaceButton::leaveEvent(QEvent *event)
{
    m_showPlus = true;
    m_plusHovered = false;
    update();
    QPushButton::leaveEvent(event);
}

void WorkspaceButton::mouseMoveEvent(QMouseEvent *event)
{
    const bool hover = rect().contains(event->pos()) && event->pos().x() >= width() - 30;
    if (hover != m_plusHovered) {
        m_plusHovered = hover;
        update();
    }
    QPushButton::mouseMoveEvent(event);
}

void WorkspaceButton::mouseReleaseEvent(QMouseEvent *event)
{
    // 点击右侧加号区域时只触发新建会话，不折叠/展开工作区
    if (m_showPlus && event->pos().x() >= width() - 30) {
        emit addSessionRequested();
        return;
    }
    QPushButton::mouseReleaseEvent(event);
}

void WorkspaceButton::paintEvent(QPaintEvent *event)
{
    QPushButton::paintEvent(event);

    if (!m_showPlus)
        return;

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

WorkspaceList::WorkspaceList(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("workspaceList"));
    setAttribute(Qt::WA_StyledBackground, true);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(2);
    m_layout->addStretch(1);
}

void WorkspaceList::setWorkspaces(const QJsonArray &items)
{
    clearWorkspaceGroups();
    m_buttons.clear();

    for (const auto &item : items) {
        const QJsonObject obj = item.toObject();
        const QString workspaceId = obj.value(QStringLiteral("workspaceId")).toString();
        const QString title = obj.value(QStringLiteral("title")).toString();
        if (workspaceId.isEmpty())
            continue;

        createWorkspaceGroup(workspaceId, title.isEmpty() ? workspaceId : title);

        const QJsonArray sessionIds = obj.value(QStringLiteral("sessionIds")).toArray();
        for (const auto &sidValue : sessionIds) {
            const QString sid = sidValue.toString();
            if (!sid.isEmpty())
                m_sessionWorkspace.insert(sid, workspaceId);
        }
    }
}

WorkspaceList::WorkspaceGroup *WorkspaceList::createWorkspaceGroup(const QString &workspaceId, const QString &title)
{
    auto *group = new WorkspaceGroup;
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
        for (SessionButton *button : group->buttons)
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

WorkspaceList::WorkspaceGroup *WorkspaceList::defaultGroup()
{
    if (!m_defaultGroup)
        m_defaultGroup = createWorkspaceGroup(QString(), QStringLiteral("未分组"));
    return m_defaultGroup;
}

void WorkspaceList::clearWorkspaceGroups()
{
    for (WorkspaceGroup *group : m_workspaceGroups) {
        if (group->container)
            delete group->container;
        delete group;
    }
    m_workspaceGroups.clear();
    m_sessionWorkspace.clear();
    m_defaultGroup = nullptr;
}

void WorkspaceList::addSession(const QString &sessionId, const QString &title)
{
    // 防止同一个会话被重复添加，避免 setCurrentSession 选中多个同 sessionId 的按钮
    for (SessionButton *button : m_buttons) {
        if (button->sessionId() == sessionId) {
            button->setSessionTitle(title);
            return;
        }
    }

    const QString workspaceId = m_sessionWorkspace.value(sessionId);
    WorkspaceGroup *group = nullptr;

    for (WorkspaceGroup *g : m_workspaceGroups) {
        if (g->workspaceId == workspaceId) {
            group = g;
            break;
        }
    }
    if (!group)
        group = defaultGroup();

    auto *button = new SessionButton(sessionId, title, group->container);
    connect(button, &SessionButton::sessionClicked, this, [this, sessionId]() {
        setCurrentSession(sessionId);
        emit sessionSelected(sessionId);
    });

    group->layout->addWidget(button);
    group->buttons.push_back(button);
    m_buttons.push_back(button);

    if (!group->expanded)
        button->setVisible(false);
}

void WorkspaceList::addSessionToWorkspace(const QString &sessionId, const QString &title, const QString &workspaceId)
{
    m_sessionWorkspace.insert(sessionId, workspaceId);
    addSession(sessionId, title);
}

void WorkspaceList::clearSessions()
{
    clearWorkspaceGroups();
    m_buttons.clear();
}

void WorkspaceList::setCurrentSession(const QString &sessionId)
{
    for (SessionButton *button : m_buttons) {
        const bool selected = button->sessionId() == sessionId;
        if (button->isChecked() != selected)
            button->setSelected(selected);
    }
}

void WorkspaceList::updateSessionTitle(const QString &sessionId, const QString &title)
{
    for (SessionButton *button : m_buttons) {
        if (button->sessionId() == sessionId) {
            button->setSessionTitle(title);
            break;
        }
    }
}

QString WorkspaceList::titleForSession(const QString &sessionId) const
{
    for (const SessionButton *button : m_buttons) {
        if (button->sessionId() == sessionId)
            return button->fullTitle();
    }
    return QString();
}

void WorkspaceList::refreshTitles(DshApiClient *api)
{
    if (!api)
        return;

    api->callMethod(
        QStringLiteral("session.list"),
        {},
        [this](const QJsonObject &value) {
            const QJsonArray items = value.value(QStringLiteral("items")).toArray();
            for (const auto &item : items) {
                const QJsonObject session = item.toObject();
                const QString sid = session.value(QStringLiteral("sessionId")).toString();
                if (sid.isEmpty())
                    continue;

                const QJsonObject projections = session.value(QStringLiteral("projections")).toObject();
                const QJsonObject values = projections.value(QStringLiteral("values")).toObject();
                QString label = values.value(QStringLiteral("title")).toString();
                if (label.isEmpty())
                    label = values.value(QStringLiteral("sessionTitle")).toString();
                if (label.isEmpty())
                    label = values.value(QStringLiteral("session.title")).toString();
                if (label.isEmpty())
                    continue;

                updateSessionTitle(sid, label);
            }
        },
        [](const DshApiClient::RpcError &) {});
}

// ------------------------------------------------------------------
// Sidebar
// ------------------------------------------------------------------

Sidebar::Sidebar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("sidebar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("#sidebar {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border-radius: 12px;") + QStringLiteral("}") + QStringLiteral("#workspaceList {") + QStringLiteral("  background: transparent;") + QStringLiteral("}") + QStringLiteral("#workspaceGroupContainer {") + QStringLiteral("  background: transparent;") + QStringLiteral("}") + QStringLiteral("QPushButton#sessionButton {") + QStringLiteral("  text-align: left;") + QStringLiteral("  padding: 0 10px;") + QStringLiteral("  background: transparent;") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QPushButton#sessionButton:hover {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("activeBg")) + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QPushButton#sessionButton:checked {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("activeBg")) + QStringLiteral(";") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("}"));

    m_logo = new SidebarLogo(this);
    m_clearButton = new ClearSessionButton(this);
    m_newWorkspaceButton = new NewWorkspaceButton(this);
    m_workspaceList = new WorkspaceList(this);
    m_settingsButton = new SidebarSettingsButton(this);
    m_pluginsButton = new SidebarPluginsButton(this);
    m_themeButton = new SidebarThemeButton(this);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(10, 10, 10, 10);
    m_layout->setSpacing(4);
    m_layout->addWidget(m_logo, 0, Qt::AlignHCenter);
    m_layout->addWidget(m_clearButton);
    m_layout->addWidget(m_newWorkspaceButton);
    m_layout->addWidget(m_workspaceList, 1);

    auto *bottomRow = new QHBoxLayout;
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->setSpacing(0);
    bottomRow->addWidget(m_settingsButton);
    bottomRow->addWidget(m_pluginsButton);
    bottomRow->addWidget(m_themeButton);
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

    connect(m_workspaceList, &WorkspaceList::sessionSelected,
            this, &Sidebar::sessionSelected);
    connect(m_workspaceList, &WorkspaceList::createSessionInWorkspaceRequested,
            this, &Sidebar::createSessionInWorkspaceRequested);
}

WorkspaceList *Sidebar::workspaceList() const
{
    return m_workspaceList;
}

void Sidebar::clearAllSessions(
    const QString &dshHome,
    const std::function<void()> &onCleared,
    const std::function<void()> &onCreateNew)
{
    // 文件处理由 Sidebar 自己负责
    if (!dshHome.isEmpty()) {
        QDir sessionsDir(dshHome + QStringLiteral("/sessions"));
        if (sessionsDir.exists()) {
            sessionsDir.removeRecursively();
            sessionsDir.mkpath(QStringLiteral("."));
        }
    }

    // 与当前加载的会话相关的清理由 WorkspaceList 负责
    if (m_workspaceList)
        m_workspaceList->clearSessions();

    if (onCleared)
        onCleared();

    if (onCreateNew)
        onCreateNew();
}