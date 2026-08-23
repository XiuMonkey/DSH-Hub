#include "SessionQuery.h"

#include "DshApiClient.h"
#include "SessionButton.h"

#include <QDir>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPixmap>
#include <QVBoxLayout>

NewSessionButton::NewSessionButton(QWidget *parent)
    : QPushButton(QStringLiteral("新建对话"), parent)
{
    setObjectName(QStringLiteral("newSessionButton"));
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(QStringLiteral(
        "QPushButton#newSessionButton {"
        "  background: #4C8BF5;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 8px;"
        "  padding: 8px 12px;"
        "  font-size: 14px;"
        "}"
        "QPushButton#newSessionButton:hover {"
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

SessionQuery::SessionQuery(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("sessionQuery"));
    // 让 QWidget 子类绘制样式表里的灰色背景
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral(
        "#sessionQuery {"
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

    // 顶部 Logo 标签，预留空间
    auto *logoLabel = new QLabel(this);
    logoLabel->setObjectName(QStringLiteral("sessionLogo"));
    logoLabel->setAlignment(Qt::AlignCenter);
    logoLabel->setAttribute(Qt::WA_TranslucentBackground);
    logoLabel->setStyleSheet(QStringLiteral("background: transparent;"));

    // 从 Qt 资源中读取 2x Logo，并按 2 倍 DPR 显示，保证高 DPI 下清晰
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
    m_newSessionButton = new NewSessionButton(this);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(10, 10, 10, 10);
    m_layout->setSpacing(4);
    m_layout->addWidget(logoLabel, 0, Qt::AlignHCenter);
    m_layout->addWidget(m_clearButton);
    m_layout->addWidget(m_newSessionButton);
    m_layout->addStretch(1);

    // 底部设置按钮：右侧预留 15px 给另一个按钮
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
    bottomRow->addSpacing(15); // 为另一个按钮预留 15px
    bottomRow->addStretch(1);

    m_layout->addLayout(bottomRow);

    connect(m_clearButton, &QPushButton::clicked,
            this, &SessionQuery::clearRequested);
    connect(m_newSessionButton, &QPushButton::clicked,
            this, &SessionQuery::newSessionRequested);
    connect(m_settingsButton, &QPushButton::clicked,
            this, &SessionQuery::settingsRequested);
}

void SessionQuery::addSession(const QString &sessionId, const QString &title)
{
    auto *button = new SessionButton(sessionId, title, this);
    connect(button, &SessionButton::sessionClicked,
            this, &SessionQuery::sessionSelected);

    // 插入到伸缩项之前（伸缩项位于底部按钮行之前）
    m_layout->insertWidget(m_layout->count() - 2, button);
    m_buttons.push_back(button);
}

void SessionQuery::clearSessions()
{
    for (SessionButton *button : m_buttons) {
        m_layout->removeWidget(button);
        delete button;
    }
    m_buttons.clear();
}

void SessionQuery::setCurrentSession(const QString &sessionId)
{
    for (SessionButton *button : m_buttons)
        button->setSelected(button->sessionId() == sessionId);
}

void SessionQuery::updateSessionTitle(const QString &sessionId, const QString &title)
{
    for (SessionButton *button : m_buttons) {
        if (button->sessionId() == sessionId) {
            button->setSessionTitle(title);
            break;
        }
    }
}

QString SessionQuery::titleForSession(const QString &sessionId) const
{
    for (const SessionButton *button : m_buttons) {
        if (button->sessionId() == sessionId)
            return button->fullTitle();
    }
    return QString();
}

void SessionQuery::refreshTitles(DshApiClient *api)
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
        [](const DshApiClient::RpcError &error) {
            Q_UNUSED(error)
        });
}

void SessionQuery::requestNewSession(
    DshApiClient *api,
    const std::function<void()> &beforeCreated,
    const std::function<void(const QString &sessionId)> &onCreated,
    const std::function<void(const QString &code, const QString &message)> &onError)
{
    if (!api)
        return;

    api->callMethod(
        QStringLiteral("session.create"),
        {},
        [this, beforeCreated, onCreated](const QJsonObject &value) {
            const QString newSessionId = value.value(QStringLiteral("sessionId")).toString();
            if (newSessionId.isEmpty())
                return;

            if (beforeCreated)
                beforeCreated();

            addSession(newSessionId, QStringLiteral("未命名会话"));
            setCurrentSession(newSessionId);

            if (onCreated)
                onCreated(newSessionId);
        },
        [onError](const DshApiClient::RpcError &error) {
            if (onError)
                onError(error.code, error.message);
        });
}

void SessionQuery::clearAllSessions(
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


