#pragma once
#include <vector>
#include <QString>
#include <QWidget>
#include "AgentMessageUnit.h"
#include "UserMessageUnit.h"
#include "SystemMessageUnit.h"

class QVBoxLayout;

class MessageQuery {
public:

struct MessageUnit
{
int Type = 0; // 0: user, 1: agent, 2: system
AgentMessageUnit* AgentUnit = nullptr;
UserMessageUnit* UserUnit = nullptr;
SystemMessageUnit* SystemUnit = nullptr;
QWidget* Container = nullptr;
};

public:
std::vector<MessageUnit> Messages;
MessageQuery();
~MessageQuery();

UserMessageUnit* addUserMessage(const QString &text, QVBoxLayout *layout);
SystemMessageUnit* addSystemMessage(const QString &text, QVBoxLayout *layout);
AgentMessageUnit* addAgentMessage(const QString &markdown, QVBoxLayout *layout,
                                  const QString &thinking = QString());
UserMessageUnit* insertUserMessage(const QString &text, QVBoxLayout *layout, int layoutIndex);
AgentMessageUnit* insertAgentMessage(const QString &markdown, QVBoxLayout *layout, int layoutIndex,
                                     const QString &thinking = QString());
AgentMessageUnit* lastAgentUnit() const;
AgentMessageUnit* lastAgentUnitIfLast() const;
void clear();

// 控件缓存支持：从布局中摘下但不销毁，之后可以重新 attach 回来
void detachFromLayout(QVBoxLayout *layout);
void attachToLayout(QVBoxLayout *layout);

// 把内部控件从当前父对象上解除，便于从临时离屏容器安全迁移
void releaseWidgets();

};