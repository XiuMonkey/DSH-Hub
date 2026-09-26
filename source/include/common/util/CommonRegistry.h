#pragma once

// 全局对象注册表：以字符串为索引登记"程序级唯一对象"，供其它模块按名取用。
// 登记是覆盖语义（后来者胜），故 Destroy 必须带对象指针做身份校验；单例刻意不析构。

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class CommonRegistry : public QObject
{
	Q_OBJECT

public:
	static CommonRegistry& instance();

	// 覆盖语义：同一 Index 已有对象就换掉；顶掉存活对象时记一条 qInfo
	bool AddToRegistry(const QString& index, QObject* obj);

	// 只有登记的指针与 obj 相同才摘除，否则不做任何改动
	bool Destroy(const QString& index, QObject* obj);

	// 不存在或已销毁都返回空 QPointer
	QPointer<QObject> FindFromRegistry(const QString& index) const;

	bool contains(const QString& index) const;

private:
	// 刻意私有：单例只能通过 instance() 获得
	CommonRegistry() = default;
	~CommonRegistry() override = default;

	QHash<QString, QPointer<QObject>> m_objects;
};
