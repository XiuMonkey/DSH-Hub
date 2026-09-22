#pragma once

// ------------------------------------------------------------------
// CommonRegistry.h
// ------------------------------------------------------------------
// 全局对象注册表：以字符串为索引登记“程序级唯一对象”，供其它模块按名字
// 取用，避免层层向下传指针，也避免各处各自持有裸指针。
//
// 设计要点：
//   - 值类型是 QPointer<QObject>：对象被销毁后自动置空，Find 永不返回
//     野指针；即使某个类漏写注销，最坏结果只是表里留下一条空记录。
//   - 登记是**覆盖**语义：同一 Index 上已有对象就换掉（后来者胜）。
//     单实例对象“新的顶掉旧的”是常态 —— 例如切主题时 ThemeManager 会先建
//     新窗口、旧窗口下一轮事件循环才析构（ThemeManager.cpp 的 setTheme 路径），
//     拒绝登记会让新对象永远进不了表、索引在旧对象析构后彻底变空。
//   - 注销必须带对象指针做身份校验：只有“表里当前登记的正是这个指针”才摘除，
//     指针不同就拒绝销毁。这条是覆盖语义能安全的前提 —— 被顶掉的旧对象随后
//     析构时，不会把新对象刚接管的记录误删。
//   - 允许同一 QObject 登记在多个 Index 下（一个对象承担多种角色）；
//     注销某个 Index 不影响该对象的其它登记项。
//   - 单例刻意不析构（见 .cpp）：注册表必须活得比所有被登记对象更久，
//     否则对象的析构函数里调用注销就会访问已析构的单例。
//
// 表里**只存对象，不存类型标签**：类型信息由调用点自己用 qobject_cast /
// Find<T>() 还原。曾一度加过 RegType 枚举用于“宿主按标签代转”，2026-09-19
// 移除 —— 那种做法需要运行期把标签映射到具体签名的函数指针，而插件侧的
// 签名是编译期固定的，二者对不上，最终只会招来一层 thunk。
//
// 用法：
//   CommonRegistry::instance().AddToRegistry("sidebar", this);
//   if (auto* sb = CommonRegistry::instance().Find<Sidebar>("sidebar")) ...
//   CommonRegistry::instance().Destroy("sidebar", this);   // 析构时
//
// 注意：只登记“整个程序生命周期内应当唯一”的界面/服务对象；临时对象、
// 一次性控件不要往里放，否则注册表会退化成什么都装的全局变量表。
// ------------------------------------------------------------------

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

	// 按 Index 登记对象。成功返回 true；index 或 obj 为空时返回 false。
	//
	// 覆盖语义：同一 Index 上已有对象时**直接换掉**（后来者胜，返回 true）。
	// 若被顶掉的是一个仍存活的对象，会记一条 qInfo 便于事后确认发生过接管。
	// 覆盖之所以安全，靠的是 Destroy 的身份校验 —— 被顶掉的对象之后析构时
	// 调 Destroy 会因指针不符而被拒绝，不会误删新记录。
	bool AddToRegistry(const QString& index, QObject* obj);

	// 按 Index 注销对象：**先校验身份** —— 只有该 Index 当前登记的指针与 obj
	// 相同时才摘除。返回是否真的摘除了；Index 不存在、或登记的已是别的对象
	// → false（且不做任何改动）。
	// 这条校验不是可选项：登记是覆盖语义，只有它才能保证“被顶掉的旧对象”
	// 之后析构时不会把新对象的记录误删。
	bool Destroy(const QString& index, QObject* obj);

	// 按 Index 取对象；不存在或已销毁都返回空 QPointer。
	QPointer<QObject> FindFromRegistry(const QString& index) const;

	// 类型化取用：内部 qobject_cast，类型不符返回 nullptr。
	template <typename T>
	T* Find(const QString& index) const
	{
		return qobject_cast<T*>(FindFromRegistry(index).data());
	}

	// 该 Index 上是否登记着一个存活的对象。
	bool contains(const QString& index) const;

	// 当前存活登记项的 Index 列表（不包含已失效的空记录），便于排查。
	QStringList liveIndexes() const;

private:
	// 刻意私有：单例只能通过 instance() 获得。
	// 放在 .cpp 里定义为不析构的堆对象，见文件头说明。
	CommonRegistry() = default;
	~CommonRegistry() override = default;

	// 值统一为 QPointer：对象销毁后自动置空
	QHash<QString, QPointer<QObject>> m_objects;
};
