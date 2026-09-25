#include "core/ConnectionManager.h"

#include "common/util/CommonRegistry.h"
#include "core/HostExports.h"

#include <utility>

ConnectionManager& ConnectionManager::instance()
{
	static ConnectionManager* const inst = new ConnectionManager();
	return *inst;
}

ConnectionManager::~ConnectionManager() = default;

void ConnectionManager::RegisterConnection(QString index, ConnectionGroup group)
{
	PublicRemoveConnection(index);
	const auto handle = QObject::connect(group.Sender, group.mSignal, group.Receiver, group.mSlot);
	ConnectionRegistry.insert(index, group);
	HandleMap.insert(index, handle);
}

void ConnectionManager::PublicRemoveConnection(QString index)
{
	const auto found1 = ConnectionRegistry.find(index);
	const auto found2 = HandleMap.find(index);
	const bool hasEntry = (found1 != ConnectionRegistry.end());
	const bool hasHandle = (found2 != HandleMap.end());
	if (!hasEntry && !hasHandle) {
		return;
	}
	// ⚠️ 两张表不总是一起有条目：只剩一张时也必须走对分支，原来的写法在那种情况下会对
	// end() 迭代器取 value()（未定义行为），拿到野指针再喂给 QObject::disconnect
	if (hasEntry && found1.value().mSlot == "Lambda") {
		if (hasHandle)
			QObject::disconnect(found2.value());
	}
	else if (hasEntry) {
		QObject::disconnect(found1.value().Sender, found1.value().mSignal, found1.value().Receiver,
			found1.value().mSlot);
	}
	if (hasEntry)
	{
		ConnectionRegistry.remove(index);
	}
	if (hasHandle)
	{
		HandleMap.remove(index);
	}
}

void ConnectionManager::SuspendConnection(QString index)
{
	const auto found1 = ConnectionRegistry.constFind(index);
	if (found1 == ConnectionRegistry.constEnd())
	{
		// 没登记过这个 index：原来的写法会直接取 found1.value()（对 end() 迭代器取值 = 未定义行为），
		// 拿到野指针再喂给 QObject::disconnect ⇒ 访问冲突（实测 0xC0000005）。插件的 index 少写一个
		// 字符就能把整个客户端崩掉，所以这里必须早退
		return;
	}
	const auto found2 = HandleMap.find(index);
	if (found1.value().mSlot == "Lambda") {
		if (found2 != HandleMap.end())
			QObject::disconnect(found2.value());
	}
	else {
		QObject::disconnect(found1.value().Sender, found1.value().mSignal, found1.value().Receiver,
			found1.value().mSlot);
	}
} // 悬挂起连接，disconnect 但不删表

void ConnectionManager::Reconnect(QString index)
{
	const auto found1 = ConnectionRegistry.constFind(index);
	if (found1 == ConnectionRegistry.constEnd())
	{
		// 同 SuspendConnection：原来对 end() 取 value() 会拿到野指针 ⇒ 访问冲突（实测 0xC0000005）
		return;
	}

	if (found1.value().mSlot == "Lambda") {
		return;
	}
	else {
		QObject::connect(found1.value().Sender, found1.value().mSignal, found1.value().Receiver, found1.value().mSlot);
	}
}

void ConnectionManager::TakeoverConnection(QString index, QObject* receiver, QByteArray slot)
{
	const auto found = ConnectionRegistry.find(index);
	if (found == ConnectionRegistry.end())
	{
		return;
	}
	SuspendConnection(index);
	RegisterConnection(index, { found.value().Sender,receiver,found.value().mSignal,slot });
}

void ConnectionManager::PrivateRemoveConnection(ConnectionGroup group)
{
	QObject::disconnect(group.Sender,group.mSignal,group.Receiver,group.mSlot);
	for (auto it = ConnectionRegistry.begin(); it != ConnectionRegistry.end(); ++it)
	{
		if (it.value().mSignal==group.mSignal &&
			it.value().mSlot==group.mSlot &&
			it.value().Sender==group.Sender &&
			it.value().Receiver==group.Receiver)
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
