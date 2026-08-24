#include "WorkspaceSessionList.h"

#include "DshApiClient.h"
#include "SessionButton.h"

#include <QDir>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QVBoxLayout>

NewWorkspaceButton::NewWorkspaceButton(QWidget *parent)
    : QPushButton(QStringLiteral("新建工作区"), parent)
{
    setObjectName(QStringLiteral("newWorkspaceButton"));
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(QStringLiteral(
        "QPushButton#newWorkspaceButton {"
        "  background: #4C8BF5;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 8px;"
        "  padding: 8px 12px;"
        "  font-size: 14px;"
        "}"
        "QPushButton#newWorkspaceButton:hover {"
        "  background: #3A7AE0;"
        "}"
    ));
}

ClearSessionButton::ClearSessionButton(QWidget *parent)
    : QPushButton(QStringLiteral("清空会话"), parent)
{
    setObjectName(QStringLiteral("clearSessionButton"));
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(QStringLiteral(
        "QPushButton#clearSessionButton {"
        "  background: #E5484D;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 8px;"
        "  padding: 8px 12px;"
        "  font-size: 14px;"
        "}"
        "QPushButton#clearSessionButton:hover {"
        "  background: #D93025;"
        "}"
    ));
}

WorkspaceButton::WorkspaceButton(const QString &title, QWidget *parent)
    : QPushButton(parent)
    , m_title(title)
{
    setObjectName(QStringLiteral("workspaceButton"));
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    setCheckable(true);
    setChecked(true);
    setStyleSheet(QStringLiteral(
        "QPushButton#workspaceButton {"
        "  background: transparent;"
        "  border: none;"
        "  border-radius: 8px;"
        "  padding: 6px 26px 6px 10px;"
        "  color: #1F2328;"
        "  font-size: 13px;"
        "  font-weight: 600;"
        "  text-align: left;"
        "}"
        "QPushButton#workspaceButton:hover {"
        "  background: #EEF0F2;"
        "}"
    ));

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

WorkspaceList::WorkspaceList(QWidget *parent)
    : QWidget(parent)
{
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

SessionList::SessionList(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("sessionList"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral(
        "#sessionList {"
        "  background: #FFFFFF;"
        "  border-radius: 12px;"
        "}"
        "QPushButton#sessionButton {"
        "  text-align: left;"
        "  padding: 0 10px;"
        "  background: transparent;"
        "  border: none;"
        "  border-radius: 8px;"
        "  color: #1F2328;"
        "}"
        "QPushButton#sessionButton:hover {"
        "  background: #EEF0F2;"
        "}"
        "QPushButton#sessionButton:checked {"
        "  background: #EEF0F2;"
        "  color: #1F2328;"
        "}"
    ));

    auto *logoLabel = new QLabel(this);
    logoLabel->setObjectName(QStringLiteral("sessionLogo"));
    logoLabel->setAlignment(Qt::AlignCenter);
    logoLabel->setAttribute(Qt::WA_TranslucentBackground);
    logoLabel->setStyleSheet(QStringLiteral("background: transparent;"));

    QPixmap logoPix(QStringLiteral(":/DSHHub/DSH-Hub-Logo-Tiny@2x.png"));
    if (!logoPix.isNull()) {
        logoPix.setDevicePixelRatio(2.0);
        logoLabel->setPixmap(logoPix);
        logoLabel->setFixedHeight(logoPix.height() / logoPix.devicePixelRatio());
    } else {
        logoLabel->setText(QStringLiteral("DSH Hub"));
        logoLabel->setFixedHeight(80);
    }

    m_clearButton = new ClearSessionButton(this);
    m_newWorkspaceButton = new NewWorkspaceButton(this);
    m_workspaceList = new WorkspaceList(this);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(10, 10, 10, 10);
    m_layout->setSpacing(4);
    m_layout->addWidget(logoLabel, 0, Qt::AlignHCenter);
    m_layout->addWidget(m_clearButton);
    m_layout->addWidget(m_newWorkspaceButton);
    m_layout->addWidget(m_workspaceList, 1);

    m_settingsButton = new QPushButton(this);
    m_settingsButton->setFixedSize(32, 32);
    m_settingsButton->setCursor(Qt::PointingHandCursor);
    m_settingsButton->setToolTip(QStringLiteral("设置"));
    m_settingsButton->setIcon(QIcon(QStringLiteral(":/DSHHub/Setting-Icon.png")));
    m_settingsButton->setIconSize(QSize(20, 20));
    m_settingsButton->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: transparent;"
        "  border: none;"
        "  border-radius: 8px;"
        "}"
        "QPushButton:hover {"
        "  background: #F3F4F6;"
        "}"
    ));

    auto *bottomRow = new QHBoxLayout;
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->setSpacing(0);
    bottomRow->addWidget(m_settingsButton);
    bottomRow->addSpacing(15);
    bottomRow->addStretch(1);

    m_layout->addLayout(bottomRow);

    connect(m_clearButton, &QPushButton::clicked,
            this, &SessionList::clearRequested);
    connect(m_newWorkspaceButton, &QPushButton::clicked,
            this, &SessionList::newWorkspaceRequested);
    connect(m_settingsButton, &QPushButton::clicked,
            this, &SessionList::settingsRequested);

    connect(m_workspaceList, &WorkspaceList::sessionSelected,
            this, &SessionList::sessionSelected);
    connect(m_workspaceList, &WorkspaceList::createSessionInWorkspaceRequested,
            this, &SessionList::createSessionInWorkspaceRequested);
}

void SessionList::setWorkspaces(const QJsonArray &items)
{
    m_workspaceList->setWorkspaces(items);
}

void SessionList::addSession(const QString &sessionId, const QString &title)
{
    m_workspaceList->addSession(sessionId, title);
}

void SessionList::addSessionToWorkspace(const QString &sessionId, const QString &title, const QString &workspaceId)
{
    m_workspaceList->addSessionToWorkspace(sessionId, title, workspaceId);
}

void SessionList::clearSessions()
{
    m_workspaceList->clearSessions();
}

void SessionList::setCurrentSession(const QString &sessionId)
{
    m_workspaceList->setCurrentSession(sessionId);
}

void SessionList::updateSessionTitle(const QString &sessionId, const QString &title)
{
    m_workspaceList->updateSessionTitle(sessionId, title);
}

QString SessionList::titleForSession(const QString &sessionId) const
{
    return m_workspaceList->titleForSession(sessionId);
}

void SessionList::refreshTitles(DshApiClient *api)
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

void SessionList::clearAllSessions(
    const QString &dshHome,
    const std::function<void()> &onCleared,
    const std::function<void()> &onCreateNew)
{
    clearSessions();

    if (!dshHome.isEmpty()) {
        QDir sessionsDir(dshHome + QStringLiteral("/sessions"));
        if (sessionsDir.exists()) {
            sessionsDir.removeRecursively();
            sessionsDir.mkpath(QStringLiteral("."));
        }
    }

    if (onCleared)
        onCleared();

    if (onCreateNew)
        onCreateNew();
}

