#include "MessageQuery.h"

#include <QObject>
#include <QClipboard>
#include <QGuiApplication>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

MessageQuery::MessageQuery() = default;

MessageQuery::~MessageQuery()
{
    clear();
}

UserMessageUnit *MessageQuery::addUserMessage(const QString &text, QVBoxLayout *layout)
{
    auto *unit = new UserMessageUnit;
    unit->setMessage(text);

    // 消息下方放复制按钮，整体右对齐
    auto *container = new QWidget;
    auto *box = new QVBoxLayout(container);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(2);

    box->addWidget(unit, 0, Qt::AlignRight);

    auto *copyButton = new QPushButton(QString(QChar(0x29C9))); // ⧉ 复制图标
    copyButton->setToolTip(QStringLiteral("复制"));
    copyButton->setFixedSize(28, 24);
    copyButton->setCursor(Qt::PointingHandCursor);
    copyButton->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  border: none;"
        "  background: transparent;"
        "  color: #666666;"
        "  font-size: 14px;"
        "  border-radius: 4px;"
        "  padding: 0px;"
        "  margin: 0px;"
        "}"
        "QPushButton:hover {"
        "  background: rgba(0,0,0,0.08);"
        "  color: #222222;"
        "}"
        "QPushButton:pressed {"
        "  background: rgba(0,0,0,0.12);"
        "}"
    ));
    QObject::connect(copyButton, &QPushButton::clicked, unit, [unit]() {
        if (!unit->toPlainText().isEmpty())
            QGuiApplication::clipboard()->setText(unit->toPlainText());
    });
    box->addWidget(copyButton, 0, Qt::AlignRight);

    layout->addWidget(container, 0, Qt::AlignRight);
    Messages.push_back(MessageUnit{0, nullptr, unit, nullptr, container});
    return unit;
}

UserMessageUnit *MessageQuery::insertUserMessage(const QString &text, QVBoxLayout *layout, int layoutIndex)
{
    auto *unit = new UserMessageUnit;
    unit->setMessage(text);

    auto *container = new QWidget;
    auto *box = new QVBoxLayout(container);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(2);

    box->addWidget(unit, 0, Qt::AlignRight);

    auto *copyButton = new QPushButton(QString(QChar(0x29C9)));
    copyButton->setToolTip(QStringLiteral("复制"));
    copyButton->setFixedSize(28, 24);
    copyButton->setCursor(Qt::PointingHandCursor);
    copyButton->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  border: none;"
        "  background: transparent;"
        "  color: #666666;"
        "  font-size: 14px;"
        "  border-radius: 4px;"
        "  padding: 0px;"
        "  margin: 0px;"
        "}"
        "QPushButton:hover {"
        "  background: rgba(0,0,0,0.08);"
        "  color: #222222;"
        "}"
        "QPushButton:pressed {"
        "  background: rgba(0,0,0,0.12);"
        "}"
    ));
    QObject::connect(copyButton, &QPushButton::clicked, unit, [unit]() {
        if (!unit->toPlainText().isEmpty())
            QGuiApplication::clipboard()->setText(unit->toPlainText());
    });
    box->addWidget(copyButton, 0, Qt::AlignRight);

    layout->insertWidget(layoutIndex, container, 0, Qt::AlignRight);
    Messages.insert(Messages.begin(), MessageUnit{0, nullptr, unit, nullptr, container});
    return unit;
}


SystemMessageUnit *MessageQuery::addSystemMessage(const QString &text, QVBoxLayout *layout)
{
    auto *unit = new SystemMessageUnit;
    unit->setMessage(text);
    layout->addWidget(unit, 0, Qt::AlignLeft);
    Messages.push_back(MessageUnit{2, nullptr, nullptr, unit, nullptr});
    return unit;
}

AgentMessageUnit *MessageQuery::addAgentMessage(const QString &markdown, QVBoxLayout *layout,
                                                const QString &thinking)
{
    // 如果上一条仍然是 Agent 消息，且中间没有被用户/系统消息打断，就合并到同一个气泡
    if (AgentMessageUnit *last = lastAgentUnitIfLast()) {
        // 每次合并新的一段输出前，先加一个段落分隔，避免多段内容挤在一起。
        // 如果这一段只有回复没有思考，appendMarkdownWithCodeShadow 内部会自己分段。
        if (!last->document()->isEmpty() && !thinking.isEmpty())
            last->appendSeparator();

        if (!thinking.isEmpty())
            last->appendThinking(thinking);
        if (!markdown.isEmpty())
            last->appendMarkdownWithCodeShadow(markdown);
        return last;
    }

    auto *unit = new AgentMessageUnit;

    if (!thinking.isEmpty())
        unit->appendThinking(thinking);

    unit->appendMarkdownWithCodeShadow(markdown);

    // 消息下方放复制按钮，整体左对齐
    auto *container = new QWidget;
    auto *box = new QVBoxLayout(container);
    box->setContentsMargins(10, 0, 0, 0);
    box->setSpacing(2);

    box->addWidget(unit, 0, Qt::AlignLeft);

    auto *copyButton = new QPushButton(QString(QChar(0x29C9))); // ⧉ 复制图标
    copyButton->setToolTip(QStringLiteral("复制"));
    copyButton->setFixedSize(28, 24);
    copyButton->setCursor(Qt::PointingHandCursor);
    copyButton->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  border: none;"
        "  background: transparent;"
        "  color: #666666;"
        "  font-size: 14px;"
        "  border-radius: 4px;"
        "  padding: 0px;"
        "  margin: 0px;"
        "}"
        "QPushButton:hover {"
        "  background: rgba(0,0,0,0.08);"
        "  color: #222222;"
        "}"
        "QPushButton:pressed {"
        "  background: rgba(0,0,0,0.12);"
        "}"
    ));
    QObject::connect(copyButton, &QPushButton::clicked, unit, [unit]() {
        if (!unit->toPlainText().isEmpty())
            QGuiApplication::clipboard()->setText(unit->toPlainText());
    });
    box->addWidget(copyButton, 0, Qt::AlignLeft);

    layout->addWidget(container, 0, Qt::AlignLeft);
    Messages.push_back(MessageUnit{1, unit, nullptr, nullptr, container});
    return unit;
}

AgentMessageUnit *MessageQuery::insertAgentMessage(const QString &markdown, QVBoxLayout *layout, int layoutIndex,
                                                   const QString &thinking)
{
    auto *unit = new AgentMessageUnit;

    if (!thinking.isEmpty())
        unit->appendThinking(thinking);

    unit->appendMarkdownWithCodeShadow(markdown);

    auto *container = new QWidget;
    auto *box = new QVBoxLayout(container);
    box->setContentsMargins(10, 0, 0, 0);
    box->setSpacing(2);

    box->addWidget(unit, 0, Qt::AlignLeft);

    auto *copyButton = new QPushButton(QString(QChar(0x29C9)));
    copyButton->setToolTip(QStringLiteral("复制"));
    copyButton->setFixedSize(28, 24);
    copyButton->setCursor(Qt::PointingHandCursor);
    copyButton->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  border: none;"
        "  background: transparent;"
        "  color: #666666;"
        "  font-size: 14px;"
        "  border-radius: 4px;"
        "  padding: 0px;"
        "  margin: 0px;"
        "}"
        "QPushButton:hover {"
        "  background: rgba(0,0,0,0.08);"
        "  color: #222222;"
        "}"
        "QPushButton:pressed {"
        "  background: rgba(0,0,0,0.12);"
        "}"
    ));
    QObject::connect(copyButton, &QPushButton::clicked, unit, [unit]() {
        if (!unit->toPlainText().isEmpty())
            QGuiApplication::clipboard()->setText(unit->toPlainText());
    });
    box->addWidget(copyButton, 0, Qt::AlignLeft);

    layout->insertWidget(layoutIndex, container, 0, Qt::AlignLeft);
    Messages.insert(Messages.begin(), MessageUnit{1, unit, nullptr, nullptr, container});
    return unit;
}


AgentMessageUnit *MessageQuery::lastAgentUnit() const
{
    for (auto it = Messages.rbegin(); it != Messages.rend(); ++it) {
        if (it->AgentUnit)
            return it->AgentUnit;
    }
    return nullptr;
}

AgentMessageUnit *MessageQuery::lastAgentUnitIfLast() const
{
    if (Messages.empty())
        return nullptr;

    // 只有最后一条消息是 Agent 消息时才返回，保证连续 Agent 回复可以合并到同一个气泡
    return Messages.back().AgentUnit;
}

void MessageQuery::detachFromLayout(QVBoxLayout *layout)
{
    for (const MessageUnit &message : Messages) {
        if (message.Container) {
            layout->removeWidget(message.Container);
            message.Container->hide();
        } else if (message.SystemUnit) {
            layout->removeWidget(message.SystemUnit);
            message.SystemUnit->hide();
        }
    }
}

void MessageQuery::attachToLayout(QVBoxLayout *layout)
{
    for (const MessageUnit &message : Messages) {
        if (message.Container) {
            const Qt::Alignment align = message.Type == 0 ? Qt::AlignRight : Qt::AlignLeft;
            layout->addWidget(message.Container, 0, align);
            message.Container->show();
        } else if (message.SystemUnit) {
            layout->addWidget(message.SystemUnit, 0, Qt::AlignLeft);
            message.SystemUnit->show();
        }
    }
}

void MessageQuery::releaseWidgets()
{
    for (const MessageUnit &message : Messages) {
        if (message.Container)
            message.Container->setParent(nullptr);
        else if (message.SystemUnit)
            message.SystemUnit->setParent(nullptr);
    }
}

void MessageQuery::clear()
{
    for (const MessageUnit &message : Messages) {
        if (message.Container) {
            // 用户/Agent 消息带容器，删除容器会连带删除里面的文本单元和复制按钮
            message.Container->deleteLater();
        } else {
            if (message.UserUnit)
                message.UserUnit->deleteLater();
            if (message.AgentUnit)
                message.AgentUnit->deleteLater();
            if (message.SystemUnit)
                message.SystemUnit->deleteLater();
        }
    }
    Messages.clear();
}
