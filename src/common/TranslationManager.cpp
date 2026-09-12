#include "TranslationManager.h"

#include "SettingsStore.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QTranslator>
#include <QXmlStreamReader>

#include <algorithm>

namespace
{
	// 翻译文件基名：dshhub_en.qm / dshhub_ja.qm
	const char* const kFileBase = "dshhub";

	// 内置默认语言包所在前缀：resources/translations/*.qm 打进 qrc 后在这里
	const char* const kResourcePrefix = ":/DSHHub/translations/";

	// 已安装的 translator（切换时先卸旧的）
	QTranslator* g_translator = nullptr;

	// 生效中的语言代码（已解析过“跟随系统”）
	QString g_activeCode;

	// 装一个 .qm。查找顺序：外部 translations/ 优先（可覆盖），
	// 失败再退回 qrc 里的内置默认语言包 —— 与样式的处理方式一致。
	// 返回实际用到的来源（"external" / "builtin"），都没装上返回空串。
	QString installFrom(const QString& fileBase, const QString& dir)
	{
		auto* translator = new QTranslator(QCoreApplication::instance());

		QString origin;
		if (translator->load(fileBase, dir))
			origin = QStringLiteral("external");
		else if (translator->load(QString::fromLatin1(kResourcePrefix) + fileBase))
			origin = QStringLiteral("builtin");

		if (origin.isEmpty()) {
			delete translator;
			return QString();
		}

		if (g_translator) {
			QCoreApplication::removeTranslator(g_translator);
			g_translator->deleteLater();
		}
		g_translator = translator;
		QCoreApplication::installTranslator(g_translator);
		return origin;
	}

	// 卸掉当前 translator（回到源语言代码里的原文）
	void uninstall()
	{
		if (!g_translator)
			return;

		QCoreApplication::removeTranslator(g_translator);
		g_translator->deleteLater();
		g_translator = nullptr;
	}

	// 把内置默认语言包释放到外部目录，使用户能直接改它（存在则不覆盖）。
	// 与 ThemeManager 释放样式模板同理：内置保底，外部可覆盖。
	void releaseBuiltinPack(const QString& fileBase, const QString& dir)
	{
		const QString target = QDir(dir).filePath(fileBase + QStringLiteral(".qm"));
		if (QFile::exists(target))
			return;

		QFile res(QString::fromLatin1(kResourcePrefix) + fileBase + QStringLiteral(".qm"));
		if (!res.open(QIODevice::ReadOnly)) {
			qWarning().noquote() << QStringLiteral("[Translation] builtin pack missing from resources:")
				<< fileBase;
			return;
		}

		const QByteArray data = res.readAll();
		res.close();

		QDir().mkpath(dir);
		QFile out(target);
		if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			out.write(data);
			out.close();
			qInfo().noquote() << "[Translation] released builtin language pack:" << target;
		}
		else {
			qWarning().noquote() << "[Translation] cannot write language pack:" << target;
		}
	}

	// 目标代码的候选顺序：en_US -> en（交给 QTranslator 自己的 fallback 也能处理，
	// 这里显式列出是为了「找回退语言」时能给出准确名字）
	QString resolvedCodeFor(const QString& languageCode, const QString& systemCode)
	{
		if (!languageCode.isEmpty())
			return languageCode;
		return systemCode.isEmpty() ? Translation::sourceLanguageCode() : systemCode;
	}
}

// ------------------------------------------------------------------
// TranslationNotifier
// ------------------------------------------------------------------

TranslationNotifier::TranslationNotifier(QObject* parent)
	: QObject(parent)
{
}

TranslationNotifier& TranslationNotifier::instance()
{
	static TranslationNotifier notifier;
	return notifier;
}

void TranslationNotifier::notifyLanguageChanged()
{
	emit languageChanged();
}

// ------------------------------------------------------------------
// Translation
// ------------------------------------------------------------------

namespace Translation
{
	QString savedLanguageCode()
	{
		return SettingsStore::loadLanguageCode();
	}

	void setSavedLanguageCode(const QString& code)
	{
		SettingsStore::saveLanguageCode(code);
	}

	QString activeLanguageCode()
	{
		return g_activeCode;
	}

	QString translationsDir()
	{
		return QCoreApplication::applicationDirPath() + QStringLiteral("/translations");
	}

	QString translationFileBase(const QString& languageCode)
	{
		if (languageCode.isEmpty())
			return QString();

		return QString::fromLatin1(kFileBase) + QLatin1Char('_') + languageCode;
	}

	bool isSourceLanguage(const QString& languageCode)
	{
		if (languageCode.isEmpty())
			return true; // 没指定 = 原文

		// 中文的任意变体（zh / zh_CN / zh_TW）在源码里就是原文，不需要 .qm
		return QLocale(languageCode).language() == QLocale::Chinese;
	}

	QVector<LanguageInfo> parseAvailableLanguages(const QStringList& fileNames)
	{
		QVector<LanguageInfo> languages;
		QStringList seen;

		const QString prefix = QString::fromLatin1(kFileBase) + QLatin1Char('_');

		for (const QString& fileName : fileNames) {
			if (!fileName.endsWith(QLatin1String(".qm"), Qt::CaseInsensitive))
				continue;
			if (!fileName.startsWith(prefix, Qt::CaseInsensitive))
				continue;

			// dshhub_en.qm -> en
			const QString code = QFileInfo(fileName).completeBaseName().mid(prefix.size());
			if (code.isEmpty() || seen.contains(code, Qt::CaseInsensitive))
				continue;

			seen.append(code);

			LanguageInfo info;
			info.code = code;
			// 展示名用该语言自己的写法：English / 日本語 / Français
			info.name = QLocale(code).nativeLanguageName();
			if (info.name.isEmpty())
				info.name = code;
			languages.append(info);
		}

		std::sort(languages.begin(), languages.end(),
			[](const LanguageInfo& a, const LanguageInfo& b) { return a.name < b.name; });
		return languages;
	}

	QVector<LanguageInfo> availableLanguages()
	{
		QVector<LanguageInfo> languages;

		// 源语言永远可选（它就是代码里的原文）
		LanguageInfo source;
		source.code = sourceLanguageCode();
		source.name = QLocale(source.code).nativeLanguageName();
		if (source.name.isEmpty())
			source.name = QStringLiteral("中文");
		source.isSource = true;
		languages.append(source);

		// 再看 translations/ 里有哪些 .qm（没装语言包时这里就是空的）
		const QDir dir(translationsDir());
		const QVector<LanguageInfo> found =
			parseAvailableLanguages(dir.entryList(QDir::Files, QDir::Name));

		for (const LanguageInfo& info : found) {
			if (isSourceLanguage(info.code))
				continue; // 中文变体不必单列
			languages.append(info);
		}

		return languages;
	}

	QString displayNameFor(const QString& languageCode)
	{
		if (isSourceLanguage(languageCode)) {
			const QString name = QLocale(sourceLanguageCode()).nativeLanguageName();
			return name.isEmpty() ? QStringLiteral("中文") : name;
		}

		const QString name = QLocale(languageCode).nativeLanguageName();
		return name.isEmpty() ? languageCode : name;
	}

	QVector<TranslationSource> translationSources()
	{
		QVector<TranslationSource> sources;

		const QDir dir(translationsDir());
		const QStringList manifests = dir.entryList(QStringList{ QStringLiteral("*.ts") }, QDir::Files, QDir::Name);

		for (const QString& manifest : manifests) {
			QFile file(dir.filePath(manifest));
			if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
				qWarning().noquote() << QStringLiteral("[Translation] cannot read manifest:")
					<< file.fileName();
				continue;
			}

			// .ts 结构：<context><name>类名</name><message><source>原文</source>…
			// 这里只取 (context, source) —— 目标译文由 QTranslator 现算，不读文件。
			QXmlStreamReader xml(&file);
			QString context;
			QString source;      // 正在收集的 <source>
			bool inSource = false;

			while (!xml.atEnd()) {
				xml.readNext();

				if (xml.isStartElement()) {
					const QStringView name = xml.name();
					if (name == QLatin1String("context")) {
						context.clear();
					}
					else if (name == QLatin1String("name") && context.isEmpty()) {
						context = xml.readElementText();
					}
					else if (name == QLatin1String("source")) {
						inSource = true;
						source.clear();
					}
				}
				else if (xml.isCharacters() && inSource) {
					// 源串可能被拆成多段字符（转义、换行），逐段拼起来
					source += xml.text().toString();
				}
				else if (xml.isEndElement() && xml.name() == QLatin1String("source")) {
					inSource = false;
					if (!source.isEmpty()) {
						TranslationSource entry;
						entry.context = context;
						entry.source = source;
						sources.append(entry);
					}
					source.clear();
				}
			}

			if (xml.hasError()) {
				qWarning().noquote() << QStringLiteral("[Translation] manifest parse error:")
					<< file.fileName() << xml.errorString();
			}
		}

		return sources;
	}

	void init()
	{
		const QString systemCode = QLocale::system().name();
		const QString wanted = resolvedCodeFor(savedLanguageCode(), systemCode);

		g_activeCode = wanted;

		const QString dir = translationsDir();
		QDir().mkpath(dir);

		// 先把内置默认语言包释放出来，用户可以就地改（存在则不覆盖）
		releaseBuiltinPack(translationFileBase(sourceLanguageCode()), dir);

		if (isSourceLanguage(wanted)) {
			// 源语言（中文）也走语言包：这样默认显示同样来自数据文件，
			// 改文案不必重编译。包里缺这条时 QTranslator 会回退到代码里的原文，
			// 所以即使内置包缺失，界面也不会变成空白。
			const QString origin = installFrom(translationFileBase(wanted), dir);
			if (origin.isEmpty()) {
				qInfo().noquote() << QStringLiteral("[Translation] default pack for")
					<< wanted << QStringLiteral("unavailable -> showing source text as-is");
			}
			return;
		}

		const QString fileBase = translationFileBase(wanted);
		const QString origin = installFrom(fileBase, dir);
		if (!origin.isEmpty())
			return;

		// 找不到对应语言包：退回源语言的语言包（再退回代码原文），原因写进日志
		qInfo().noquote() << QStringLiteral("[Translation] no language pack for")
			<< wanted << QStringLiteral("under") << dir
			<< QStringLiteral("-> falling back to the default language");
		g_activeCode = sourceLanguageCode();
		uninstall();
		installFrom(translationFileBase(sourceLanguageCode()), dir);
	}

	bool apply(const QString& languageCode)
	{
		const QString systemCode = QLocale::system().name();
		const QString wanted = resolvedCodeFor(languageCode, systemCode);

		// 换 translator 之前先拍一份“界面上正显示的旧文案 -> 源串”的快照：
		// 只有在旧语言下才能把当前文案认出来（换完就对不上了）。
		const WidgetTextSnapshot snapshot = snapshotWidgetTexts();

		const QString dir = translationsDir();
		QDir().mkpath(dir);
		releaseBuiltinPack(translationFileBase(sourceLanguageCode()), dir);

		// 源语言与其它语言走同一条路径：能装上语言包就用它，装不上就退回代码原文
		const QString origin = installFrom(translationFileBase(wanted), dir);
		const bool switched = !origin.isEmpty();

		if (!switched) {
			qWarning().noquote() << QStringLiteral("[Translation] language pack not found for")
				<< wanted << QStringLiteral("under") << dir
				<< QStringLiteral("-> falling back to the default language");
			uninstall();
			installFrom(translationFileBase(sourceLanguageCode()), dir);
		}

		const QString active = switched ? wanted : sourceLanguageCode();
		const bool changed = (active != g_activeCode);
		g_activeCode = active;

		// 装/卸 translator 时 Qt 自己会给所有控件发 LanguageChange，
		// 已按标准做法接了 changeEvent 的控件会自己刷新；
		// 剩下的固定文案靠快照就地换掉（见 TranslationUi.cpp 的说明与局限）。
		applyWidgetTextSnapshot(snapshot);

		// 再通知一次非控件对象（重复刷新是幂等的，代价很小）
		TranslationNotifier::instance().notifyLanguageChanged();

		return changed;
	}
}
