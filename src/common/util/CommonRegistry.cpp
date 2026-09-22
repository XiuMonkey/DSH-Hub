#include "common/util/CommonRegistry.h"

CommonRegistry& CommonRegistry::instance()
{
	// 刻意用 new 且不 delete：注册表的生命周期必须晚于所有被登记对象。
	// 若写成函数局部 static 对象，其析构发生在 main 返回之后、顺序不可控，
	// 一旦它先于某些被登记对象析构，那些对象的析构函数里调用 Destroy()
	// 就会访问已析构的单例。这里让它在整个进程期间保持有效。
	static CommonRegistry* s_registry = new CommonRegistry;
	return *s_registry;
}

bool CommonRegistry::AddToRegistry(const QString& index, QObject* obj)
{
	if (index.isEmpty() || !obj)
		return false;

	auto it = m_objects.find(index);
	if (it == m_objects.end())
	{
		m_objects.insert(index, QPointer<QObject>(obj));
		return true;
	}

	// 同一 Index 上已有对象（存活与否都一样）：直接覆盖，后来者胜。
	// 覆盖存活对象时记一条 qInfo —— 这不是错误（切主题时新旧窗口的交接就
	// 走在这条路上），但事后排查"某个 index 什么时候换了主人"时很有用。
	QObject* const previous = it.value().data();
	it.value() = obj;
	if (previous && previous != obj)
	{
		qInfo("CommonRegistry: index \"%s\" taken over by %p (was %p)",
			qPrintable(index),
			static_cast<void*>(obj),
			static_cast<void*>(previous));
	}
	return true;
}

bool CommonRegistry::Destroy(const QString& index, QObject* obj)
{
	if (index.isEmpty() || !obj)
		return false;

	auto it = m_objects.find(index);
	if (it == m_objects.end())
		return false;

	// 身份校验（覆盖语义的安全前提）：先比对"注销请求者"与"表里现有指针"，
	// 不一样就拒绝销毁。这样"新对象已接管、旧对象才析构"的顺序不会误删新记录。
	if (it.value().data() != obj)
		return false;

	m_objects.erase(it);
	return true;
}

QPointer<QObject> CommonRegistry::FindFromRegistry(const QString& index) const
{
	const auto it = m_objects.constFind(index);
	if (it == m_objects.constEnd())
		return QPointer<QObject>();
	// QPointer 在对象销毁后自动为空，这里直接返回即为安全值。
	return it.value();
}

bool CommonRegistry::contains(const QString& index) const
{
	const auto it = m_objects.constFind(index);
	return it != m_objects.constEnd() && !it.value().isNull();
}