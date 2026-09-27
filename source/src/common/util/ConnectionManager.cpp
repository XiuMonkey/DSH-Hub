#include "common/util/ConnectionManager.h"

#include "common/util/CommonRegistry.h"
#include "ExtensionSystem/HostExports.h"

#include <utility>

// VirtualConnectionManager 的六个接口方法全部内联在 ConnectionManager.h，本文件只留单例、析构与两个内部删除助手
ConnectionManager& ConnectionManager::instance()
{
	static ConnectionManager* const inst = new ConnectionManager();
	return *inst;
}

// 补前缀：Qt 字符串版 connect 靠首字符认类型（'2' 信号 / '1' 槽），缺了不报错，
// 只打一行警告就返回空连接 —— 插件侧看不出来，订阅会静默失效。
QByteArray ConnectionManager::EnsureSignalPrefix(QByteArray signal)
{
	if (!signal.isEmpty() && signal.at(0) != '2')
		signal.prepend('2');
	return signal;
}

QByteArray ConnectionManager::EnsureSlotPrefix(QByteArray slot)
{
	// 首字符是 '2' 的不动：信号连信号时槽位本来就该带 '2'
	if (!slot.isEmpty() && slot.at(0) != '1' && slot.at(0) != '2')
		slot.prepend('1');
	return slot;
}

ConnectionManager::~ConnectionManager() = default;

void ConnectionManager::PrivateRemoveConnection(ConnectionGroup group)
{
	QObject::disconnect(group.Sender, group.mSignal, group.Receiver, group.mSlot);
	for (auto it = ConnectionRegistry.begin(); it != ConnectionRegistry.end(); ++it)
	{
		if (it.value().mSignal == group.mSignal &&
			it.value().mSlot == group.mSlot &&
			it.value().Sender == group.Sender &&
			it.value().Receiver == group.Receiver)
		{
			ConnectionRegistry.remove(it.key());
			HandleMap.remove(it.key());
			break;
		}
	}
}

void ConnectionManager::PrivateRemoveConnection(QMetaObject::Connection handle)
{
	QObject::disconnect(handle);
	for (auto it = HandleMap.begin(); it != HandleMap.end(); ++it)
	{
		if (it.value() == handle)
		{
			ConnectionRegistry.remove(it.key());
			HandleMap.remove(it.key());
			break;
		}
	}
}