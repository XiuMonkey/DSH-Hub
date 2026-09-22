#pragma once

// 全局对象注册表：以字符串为索引登记“程序级唯一对象”（只放全程序唯一的界面/服务对象），供其它模块按名取用，免去层层向下传指针。
// ⚠️ 登记是覆盖语义（后来者胜，切主题时新旧窗口会短暂并存），因此 Destroy 必须带对象指针做身份校验，否则被顶掉的旧对象析构时会误删新记录。
// ⚠️ 单例刻意不析构（必须活得比所有被登记对象更久）。用法：CommonRegistry::instance().AddToRegistry("sidebar", this) / FindFromRegistry("sidebar") / Destroy("sidebar", this)。

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

	// 登记对象（覆盖语义：同一 Index 上已有对象就换掉，后来者胜）；index 或 obj 为空返回 false，顶掉存活对象时记一条 qInfo 便于事后确认。
	bool AddToRegistry(const QString& index, QObject* obj);

	// 注销对象：只有该 Index 当前登记的指针与 obj 相同时才摘除并返回 true；Index 不存在或登记的已是别的对象 → false 且不做任何改动（这条身份校验是覆盖语义能安全的前提）。
	bool Destroy(const QString& index, QObject* obj);

	// 按 Index 取对象；不存在或已销毁都返回空 QPointer。
	QPointer<QObject> FindFromRegistry(const QString& index) const;

	bool contains(const QString& index) const;

private:
	// 刻意私有：单例只能通过 instance() 获得；在 .cpp 里定义为不析构的堆对象（原因见文件头）。
	CommonRegistry() = default;
	~CommonRegistry() override = default;

	// 值统一为 QPointer：对象销毁后自动置空
	QHash<QString, QPointer<QObject>> m_objects;
};
