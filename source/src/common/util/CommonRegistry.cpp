#include "common/util/CommonRegistry.h"

CommonRegistry& CommonRegistry::instance()
{
	// 刻意用 new 且不 delete：单例必须活得比所有被登记对象久，否则 Destroy() 会访问已析构的单例
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

	// 同一 Index 已有对象就覆盖，后来者胜；覆盖存活对象时记一条 qInfo
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

	// 身份校验：请求者与表里指针不一致即拒绝，防旧对象析构时误删新记录
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
	return it.value();
}

bool CommonRegistry::contains(const QString& index) const
{
	const auto it = m_objects.constFind(index);
	return it != m_objects.constEnd() && !it.value().isNull();
}
