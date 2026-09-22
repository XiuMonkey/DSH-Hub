#pragma once

// 多语言（Qt 原生链路，ID-based）：qtTrId("key") -> lupdate -> .ts -> lrelease -> .qm -> QTranslator；.ts 里只写 <message id="key"> + <translation>，id 是查表主键，<source> 不参与查表也不在运行期使用。
// 陷阱：源码里没有任何自然语言文案，每条文案用全局唯一 key 引用，因此所有语言（含出厂默认的中文）都需要语言包、没有“某个语言免翻译”的例外；查不到译文时兜底是 key 本身，界面会显示 topbar_settings 这样的代号 —— 这是刻意保留的：缺哪条一眼可见。
// 语言包从 exe 同目录 translations/ 读，装不上再退回 qrc 内置（内置包启动时释放到外部目录、存在则不覆盖）；切换立即生效（免重启）：apply() 换掉 translator 后 Qt 给所有控件发 QEvent::LanguageChange，不是控件、收不到该事件的对象接 TranslationNotifier::languageChanged。

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct LanguageInfo
{
	QString code; // 语言代码（"en"、"ja"…）；空串代表“跟随系统”
	QString name; // 展示名，用该语言自己的写法（"English"、"日本語"）
};

// .ts 清单里的一条记录，用于“就地换文案”（见 Translation::snapshotWidgetTexts）；ID-based 模式下查表主键是 id 而不是原文，所以这里必须带 id。
struct TranslationSource
{
	QString id;   // 查表主键：qtTrId("...") 的参数
};

// 语言切换通知：不是控件、收不到 QEvent::LanguageChange 的对象接这个信号
class TranslationNotifier : public QObject
{
	Q_OBJECT

public:
	static TranslationNotifier& instance();

	void notifyLanguageChanged();

signals:

private:
	explicit TranslationNotifier(QObject* parent = nullptr);
};

namespace Translation
{
	// 出厂默认语言：缺语言包时回退到它；key 化之后没有任何语言“免翻译”，它同样需要语言包（源码里的兜底是 key 本身，不是中文）。
	inline QString defaultLanguageCode() { return QStringLiteral("zh_CN"); }

	// 设置里保存的语言代码；空串 = 跟随系统
	QString savedLanguageCode();
	void setSavedLanguageCode(const QString& code);

	// 语言代码 -> 翻译文件基名（"en" -> "dshhub_en"）；空代码返回空串
	QString translationFileBase(const QString& languageCode);

	// 该代码是否属于默认语言（中文的任意变体都算：zh、zh_CN、zh_TW）；只用于语言列表去重，不代表“无需翻译”。
	bool isDefaultLanguage(const QString& languageCode);

	// 从文件名列表里解析出可用语言（纯函数，便于单测）：只认 dshhub_<code>.qm 这一种命名，同名去重、按名称排序。
	QVector<LanguageInfo> parseAvailableLanguages(const QStringList& fileNames);

	// 可选语言：源语言（中文）在最前，其余按 .qm 出现情况列出
	QVector<LanguageInfo> availableLanguages();

	// 安装初始翻译。main() 里在 QApplication 之后、创建窗口之前调用。
	void init();

	// 立即切换语言（免重启）；languageCode 为空表示跟随系统。返回是否真的换了（没找到对应 .qm 时会退回源语言并返回 false）。
	bool apply(const QString& languageCode);

	// exe 同目录的 translations/
	QString translationsDir();

	// 从随程序分发的 .ts 清单里读出全部 id；清单缺失时返回空（此时“就地换文案”自动降级为不做，不影响其它功能）。
	QVector<TranslationSource> translationSources();

	// 就地换文案，让“构造时写死文案”的界面也能免重启切语言：换 translator 之前用当前 translator 建立「界面此刻显示的文案 -> id」映射，换好之后遍历所有控件替换命中项；用 arg() 拼出来的动态文案与已渲染进 HTML 的历史消息不在覆盖范围内，需各自的重建路径。
	using WidgetTextSnapshot = QHash<QString, TranslationSource>;

	WidgetTextSnapshot snapshotWidgetTexts();
	void applyWidgetTextSnapshot(const WidgetTextSnapshot& snapshot);
}
