#pragma once

// 信号槽登记表：按 index 管理 connect，支持取消 / 挂起 / 接管 / 重连。
// QObject 必须是第一个基类（moc 假定首个基类即 QObject 基类）；文末 dshRegister 是宿主内部短接入口。
// 匿名性：插件的 QObject* 元对象里含全部信号，任何"插件自定 signal 去 connect"的入口都等于
// 交出宿主整套信号面。所以 RegisterConnection 只在宿主内部用，插件侧新增订阅只走
// ProtectedRegisterConnection，由它按白名单放行。

#include "VirtualClass/VirtualCommon.h"

#include <QByteArray>
#include <QHash>
#include <QMetaMethod>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <algorithm>
#include <utility>
#include <vector>

class ConnectionManager : public QObject, public VirtualConnectionManager
{
	Q_OBJECT
	Q_INTERFACES(VirtualConnectionManager)

	// 插件被允许新增订阅的信号白名单（QMetaObject 规范化签名）：Sidebar::clearRequested、
	// DSHHub::aboutToClose、DshApiClient::takeoverChanged。
	// 加条目 = 承诺该信号名稳定；存的是签名串不是 index（拿 index 比会全拒，踩过）。
	std::vector<QByteArray> m_publicSignals = {
		QByteArrayLiteral("2clearRequested()"),
		QByteArrayLiteral("2aboutToClose()"),
		QByteArrayLiteral("2takeoverChanged(bool)")
	};

public:
	static ConnectionManager& instance();
	~ConnectionManager() override;

	// 插件侧的新增订阅入口：signal 不在白名单就拒绝并记一行警告（不静默），在则转内部登记。
	// signal 是原生签名串（"2clearRequested()"），与白名单同一种串；index 只用来事后取消订阅。
	void ProtectedRegisterConnection(QString index, const QObject* sender, QByteArray signal, const QObject* receiver, const char* slot) override
	{
		const QByteArray signalName = EnsureSignalPrefix(signal);
		if (std::find(m_publicSignals.begin(), m_publicSignals.end(), signalName) == m_publicSignals.end()) {
			qWarning("ProtectedRegisterConnection(%s): 信号 %s 未公开，已拒绝", qPrintable(index), signalName.constData());
			return;
		}
		// slotName 只活到本函数结束，但 constData() 只喂给同步返回的 RegisterConnection，不会存下来
		const QByteArray slotName = EnsureSlotPrefix(QByteArray(slot));
		RegisterConnection(index, sender, signalName, receiver, slotName.constData());
	}

	// 宿主内部登记（**不上虚接口**）：调用方自己提供 sender + signal，不做任何校验
	void RegisterConnection(QString index, ConnectionGroup group)
	{
		PublicRemoveConnection(index);
		group.mSignal = EnsureSignalPrefix(group.mSignal);
		group.mSlot = EnsureSlotPrefix(group.mSlot);
		const auto handle = QObject::connect(group.Sender, group.mSignal, group.Receiver, group.mSlot);
		if (!handle) {
			// 连不上就别登记：登记了只会让 Suspend / Reconnect 拿空 handle 空转，而调用方
			// 拿不到返回值，只能靠这行日志 —— 少写它，故障就是彻底静默的
			qWarning("RegisterConnection(%s): 连接未建立（signal=%s），未登记", qPrintable(index), group.mSignal.constData());
			return;
		}
		ConnectionRegistry.insert(index, group);
		HandleMap.insert(index, handle);
	}
	template <typename Func>
	void RegisterConnection(QString index, const QObject* sender, QByteArray signal, const QObject* receiver, Func slot)
	{
		PublicRemoveConnection(index);
		const auto handle = QObject::connect(sender, signal, receiver, slot);
		if (!handle) {
			qWarning("RegisterConnection(%s): 连接未建立（signal=%s），未登记", qPrintable(index), signal.constData());
			return;
		}
		ConnectionRegistry.insert(index, { const_cast<QObject*>(sender), const_cast<QObject*>(receiver), signal, "Lambda" });
		HandleMap.insert(index, handle);
	}
	template <typename Sender, typename Signal, typename Func>
	void RegisterConnection(QString index, const Sender* sender, Signal signal, const QObject* receiver, Func slot)
	{
		PublicRemoveConnection(index);
		const auto handle = QObject::connect(sender, signal, receiver, slot);
		if (!handle) {
			qWarning("RegisterConnection(%s): 连接未建立（成员函数指针形式），未登记", qPrintable(index));
			return;
		}
		ConnectionRegistry.insert(index, { const_cast<Sender*>(sender), const_cast<QObject*>(receiver), QMetaMethod::fromSignal<Signal>(signal).methodSignature(), "Lambda" });
		HandleMap.insert(index, handle);
	}
	void PublicRemoveConnection(QString index) override
	{
		const auto found1 = ConnectionRegistry.find(index);
		const auto found2 = HandleMap.find(index);
		const bool hasEntry = (found1 != ConnectionRegistry.end());
		const bool hasHandle = (found2 != HandleMap.end());
		if (!hasEntry && !hasHandle) {
			return;
		}
		// 两张表不总是一起有条目：只剩一张时也必须走对分支，否则会对 end() 迭代器取 value()（未定义行为），
		// 拿到野指针再喂给 QObject::disconnect
		if (hasEntry && found1.value().mSlot == "Lambda") {
			if (hasHandle)
				QObject::disconnect(found2.value());
		}
		else if (hasEntry) {
			QObject::disconnect(found1.value().Sender, found1.value().mSignal, found1.value().Receiver, found1.value().mSlot);
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

	// 悬挂起连接：disconnect 但不删表
	void SuspendConnection(QString index) override
	{
		const auto found1 = ConnectionRegistry.constFind(index);
		if (found1 == ConnectionRegistry.constEnd())
		{
			// 没登记过这个 index 必须早退：对 end() 取 value() = 未定义行为，拿到野指针再 disconnect
			// 就是访问冲突（实测 0xC0000005）—— 插件的 index 少写一个字符就能崩掉整个客户端
			return;
		}
		const auto found2 = HandleMap.find(index);
		if (found1.value().mSlot == "Lambda") {
			if (found2 != HandleMap.end())
				QObject::disconnect(found2.value());
		}
		else {
			QObject::disconnect(found1.value().Sender, found1.value().mSignal, found1.value().Receiver, found1.value().mSlot);
		}
	}

	void Reconnect(QString index) override
	{
		const auto found1 = ConnectionRegistry.constFind(index);
		if (found1 == ConnectionRegistry.constEnd())
		{
			// 同 SuspendConnection：未登记的 index 一律早退
			return;
		}

		// Lambda 连接只能由登记方自己重连，这里无从下手
		if (found1.value().mSlot == "Lambda") {
			return;
		}
		else {
			QObject::connect(found1.value().Sender, found1.value().mSignal, found1.value().Receiver, found1.value().mSlot);
		}
	}

	void TakeoverConnection(QString index, QObject* receiver, QByteArray slot) override
	{
		const auto found = ConnectionRegistry.find(index);
		if (found == ConnectionRegistry.end())
		{
			return;
		}
		SuspendConnection(index);
		RegisterConnection(index, { found.value().Sender, receiver, found.value().mSignal, slot });
	}

private:
	// Qt 字符串版 connect 靠首字符认类型：'2' = 信号、'1' = 槽。缺前缀**不报错**，只打一行
	// "Use the SLOT or SIGNAL macro" 就返回空连接 —— 静默失效，插件从 void 接口看不出来。
	// 只在登记入口补齐：调用方写裸名也照样连上（「清空会话」订阅就这么断过一整天）。
	static QByteArray EnsureSignalPrefix(QByteArray signal);
	static QByteArray EnsureSlotPrefix(QByteArray slot);

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
