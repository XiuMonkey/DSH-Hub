#include "core/ConnectionManager.h"

#include "common/util/CommonRegistry.h"
#include "core/HostExports.h"

#include <utility>

// VirtualConnectionManager 的五个接口方法全部内联在 ConnectionManager.h，本文件只留单例、析构与两个内部删除助手
ConnectionManager& ConnectionManager::instance()
{
	static ConnectionManager* const inst = new ConnectionManager();
	return *inst;
}

ConnectionManager::~ConnectionManager() = default;

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
