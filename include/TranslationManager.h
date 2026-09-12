#pragma once

// ------------------------------------------------------------------
// TranslationManager.h
// ------------------------------------------------------------------
// 多语言（Qt 原生链路）：tr() -> lupdate -> .ts -> lrelease -> .qm -> QTranslator
//
// 源语言是中文：代码里的字面量本身就是中文，所以「中文」不需要任何 .qm ——
// 不装 translator 时显示的就是原文。中文之外的语言各出一份 dshhub_<code>.qm。
//
// 翻译文件只从 exe 同目录的 translations/ 读（不嵌 qrc）：这样补翻译、改翻译
// 都不用重新编译，与 ThemeManager 让样式可外部覆盖是同一个思路。
//
// 切换是立即生效的（免重启）：apply() 换掉 translator 之后 Qt 会给所有控件发
// QEvent::LanguageChange，各界面在自己的 changeEvent 里重新设置文案即可。
// 不是控件、收不到该事件的对象（例如持有展示文案的逻辑类）接
// TranslationNotifier::languageChanged。
// ------------------------------------------------------------------

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

// 一个可选语言
struct LanguageInfo
{
	QString code;          // 语言代码（"en"、"ja"…）；空串代表“跟随系统”
	QString name;          // 展示名，用该语言自己的写法（"English"、"日本語"）
	bool isSource = false; // 是否为源语言（中文，无需 .qm）
};

// .ts 清单里的一条 (上下文, 源串)。用于“就地换文案”，见 Translation::snapshotWidgetTexts。
struct TranslationSource
{
	QString context;
	QString source;
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
	// 源语言：代码字面量就是中文，选它 = 不装 translator
	inline QString sourceLanguageCode() { return QStringLiteral("zh_CN"); }

	// 设置里保存的语言代码；空串 = 跟随系统
	QString savedLanguageCode();
	void setSavedLanguageCode(const QString& code);

	// 生效中的语言代码（已解析过“跟随系统”，不会是空串）
	QString activeLanguageCode();

	// 语言代码 -> 翻译文件基名（"en" -> "dshhub_en"）；空代码返回空串
	QString translationFileBase(const QString& languageCode);

	// 该代码是否属于源语言（中文的任意变体都算：zh、zh_CN、zh_TW）
	bool isSourceLanguage(const QString& languageCode);

	// 从文件名列表里解析出可用语言（纯函数，便于单测）。
	// 只认 dshhub_<code>.qm 这一种命名；同名去重，按名称排序。
	QVector<LanguageInfo> parseAvailableLanguages(const QStringList& fileNames);

	// 可选语言：源语言（中文）在最前，其余按 .qm 出现情况列出
	QVector<LanguageInfo> availableLanguages();

	// 找一个可用语言的展示名；找不到回退成代码本身
	QString displayNameFor(const QString& languageCode);

	// 安装初始翻译。main() 里在 QApplication 之后、创建窗口之前调用。
	void init();

	// 立即切换语言（免重启）。languageCode 为空表示跟随系统。
	// 返回是否真的换了（没找到对应 .qm 时会退回源语言并返回 false）。
	bool apply(const QString& languageCode);

	// exe 同目录的 translations/
	QString translationsDir();

	// 从随程序分发的 .ts 清单里读出全部 (上下文, 源串) 对。
	// 清单缺失时返回空（此时“就地换文案”自动降级为不做，不影响其它功能）。
	QVector<TranslationSource> translationSources();

	// ------------------------------------------------------------------
	// 就地换文案：让“构造时写死文案”的界面也能免重启切语言
	// ------------------------------------------------------------------
	// 背景：Qt 的标准做法是每个界面写 retranslateUi() 并在 changeEvent 里响应
	// QEvent::LanguageChange。本项目界面多、文案散（200+ 条），逐个手写既费工又容易漏。
	//
	// 这里换一种等价的机械化做法：
	//   1) 换 translator 之前，用**当前**的 translator 把所有已知源串翻一遍，
	//      建立「界面此刻显示的文案 -> (上下文, 源串)」映射（snapshotWidgetTexts）；
	//   2) 换好 translator 之后，遍历所有控件，把命中映射的文案换成新语言的译法
	//      （applyWidgetTextSnapshot）。
	// 已按标准做法接了 changeEvent 的控件（TitleBar 等）会先自己刷新，
	// 它们的文案已变成新语言，因而不会再命中旧文案映射，不会重复翻译。
	//
	// 明确不覆盖（不假装做到）：
	//   - 用 arg() 拼出来的动态文案（如“共 3 个模型，1 个提供方。”）不是整串源码，
	//     匹配不上；它们随数据刷新重建；
	//   - 已渲染进 HTML 的历史消息（如思考块标题）。
	// 这两处若也要随语言变，需要各自的重建/重渲染路径。
	using WidgetTextSnapshot = QHash<QString, TranslationSource>;

	WidgetTextSnapshot snapshotWidgetTexts();
	void applyWidgetTextSnapshot(const WidgetTextSnapshot& snapshot);
}
