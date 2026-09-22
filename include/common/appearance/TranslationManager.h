#pragma once

// 多语言（Qt 原生链路，ID-based）：qtTrId("key") -> lupdate -> .ts -> lrelease -> .qm -> QTranslator；.ts 里只写 <message id="key"> + <translation>，id 是查表主键，<source> 不参与查表也不在运行期使用（正式 .ts 不维护，写了也会被 tools/release-translations.ps1 删掉），原文由 zh_CN 的 <translation> 承担。
// 陷阱：源码里没有任何自然语言文案，每条文案用全局唯一 key 引用（如 topbar_settings），因此所有语言（含出厂默认的中文）都需要语言包、没有“某个语言免翻译”的例外；查不到译文时兜底是 key 本身（qtTrId 对未知 id 原样返回），界面会显示 topbar_settings 这样的代号 —— 这是刻意保留的：缺哪条一眼可见，唯一例外见 TranslationUi.cpp（空译文会被挡下），改文案必须同步更新语言包。
// 语言包从 exe 同目录 translations/ 读，装不上再退回 qrc 内置（内置包启动时释放到外部目录、存在则不覆盖）；切换立即生效（免重启）：apply() 换掉 translator 后 Qt 给所有控件发 QEvent::LanguageChange，不是控件、收不到该事件的对象接 TranslationNotifier::languageChanged。

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

// 一个可选语言
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

	// 交给 Translation::apply() 在装好 translator 之后调用
	void notifyLanguageChanged();

signals:
	void languageChanged();

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

	// 生效中的语言代码（已解析过“跟随系统”，不会是空串）
	QString activeLanguageCode();

	// 语言代码 -> 翻译文件基名（"en" -> "dshhub_en"）；空代码返回空串
	QString translationFileBase(const QString& languageCode);

	// 该代码是否属于默认语言（中文的任意变体都算：zh、zh_CN、zh_TW）；只用于语言列表去重（默认语言单独占一个槽位），不代表“无需翻译”。
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

	// 就地换文案，让“构造时写死文案”的界面也能免重启切语言：换 translator 之前用当前 translator 把 .ts 里所有 id 翻一遍，建立「界面此刻显示的文案 -> id」映射（snapshotWidgetTexts）；换好之后遍历所有控件，把命中映射的文案换成新语言的译法（applyWidgetTextSnapshot）。已按标准做法接了 changeEvent 的控件（TitleBar 等）会先自己刷新，它们的文案已变成新语言、因而不会再命中旧文案映射，不会重复翻译。
	// 明确不覆盖（不假装做到）：用 arg() 拼出来的动态文案（如“共 3 个模型，1 个提供方。”）不是整串文案、匹配不上，它们随数据刷新重建；已渲染进 HTML 的历史消息（如思考块标题）—— 这两处若也要随语言变，需要各自的重建/重渲染路径。
	using WidgetTextSnapshot = QHash<QString, TranslationSource>;

	WidgetTextSnapshot snapshotWidgetTexts();
	void applyWidgetTextSnapshot(const WidgetTextSnapshot& snapshot);
}
