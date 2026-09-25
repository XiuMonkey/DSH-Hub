#pragma once

// 信号槽登记表：按 index 管理 connect，支持取消 / 挂起 / 接管 / 重连。
// QObject 必须是第一个基类（moc 假定首个基类即 QObject 基类）。
// 文末的 dshRegister 是调用点的短接入口，用法见那里的注释。

#include "VirtualClass/VirtualCommon.h"

#include <QHash>
#include <QMetaMethod>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <utility>

class ConnectionManager : public QObject, public VirtualConnectionManager
{
	Q_OBJECT
	Q_INTERFACES(VirtualConnectionManager)

public:
	static ConnectionManager& instance();
	~ConnectionManager() override;

	void RegisterConnection(QString index, ConnectionGroup group) override;
	template <typename Func>
	void RegisterConnection(QString index, const QObject* sender, QByteArray signal,const QObject* receiver, Func slot)
	{
		PublicRemoveConnection(index);
		const auto handle = QObject::connect(sender, signal, receiver, slot);
		ConnectionRegistry.insert(index, { const_cast<QObject*>(sender),const_cast<QObject*>(receiver), signal, "Lambda" });
		HandleMap.insert(index, handle);
	}
	template <typename Sender, typename Signal, typename Func>
	void RegisterConnection(QString index, const Sender* sender, Signal signal,const QObject* receiver, Func slot)
	{
		PublicRemoveConnection(index);
		const auto handle = QObject::connect(sender, signal, receiver, slot);
		ConnectionRegistry.insert(index, { const_cast<Sender*>(sender),const_cast<QObject*>(receiver),QMetaMethod::fromSignal<Signal>(signal).methodSignature(), "Lambda" });
		HandleMap.insert(index, handle);
	}
	void PublicRemoveConnection(QString index) override;
	void SuspendConnection(QString index) override;
	void TakeoverConnection(QString index, QObject* receiver, QByteArray slot) override;
	void Reconnect(QString index) override;

private:
	void PrivateRemoveConnection(ConnectionGroup group);
	void PrivateRemoveConnection(QMetaObject::Connection handle);

	QHash<QString, ConnectionGroup> ConnectionRegistry;
	QHash<QString, QMetaObject::Connection> HandleMap;
};

template <typename... Args>
inline void dshRegister(const char* index, Args&&... args)
{
	ConnectionManager::instance().RegisterConnection(QString::fromUtf8(index), std::forward<Args>(args)...);
}

template <typename... Args>
inline void dshRegister(const QString& index, Args&&... args)
{
	ConnectionManager::instance().RegisterConnection(index, std::forward<Args>(args)...);
}
