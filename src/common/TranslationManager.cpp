#include "TranslationManager.h"

#include "ClientSettings.h"

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

	// 随程序内置的语言包基名（必须与 resources/DSHHub.qrc 里的条目一致）
	const char* const kBuiltinPacks[] = { "dshhub_zh_CN", "dshhub_en" };

	// 释放全部内置语言包：内置保底，外部可覆盖（已存在的文件不动）
	void releaseBuiltinPacks(const QString& dir)
	{
		for (const char* base : kBuiltinPacks)
			releaseBuiltinPack(QString::fromLatin1(base), dir);
	}

	// 目标代码的候选顺序：en_US -> en（交给 QTranslator 自己的 fallback 也能处理，
	// 这里显式列出是为了「找回退语言」时能给出准确名字）
	QString resolvedCodeFor(const QString& languageCode, const QString& systemCode)
	{
		if (!languageCode.isEmpty())
			return languageCode;
		return systemCode.isEmpty() ? Translation::defaultLanguageCode() : systemCode;
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
		// 语言存运行目录的 ClientSetting/AppearanceSetting.json（不是注册表）
		return AppearanceSetting::languageCode();
	}

	void setSavedLanguageCode(const QString& code)
	{
		AppearanceSetting::setLanguageCode(code);
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

	bool isDefaultLanguage(const QString& languageCode)
	{
		if (languageCode.isEmpty())
			return true; // 没指定 = 默认语言

		// 中文的任意变体（zh / zh_CN / zh_TW）都归入默认语言这一个槽位。
		// 注意这只影响语言列表的去重，不代表中文"不需要语言包"。
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

		// 默认语言（中文）永远可选：它由 qrc 内置包保底，外部包被删也会自动释放回来
		LanguageInfo fallback;
		fallback.code = defaultLanguageCode();
		fallback.name = QLocale(fallback.code).nativeLanguageName();
		// 源码里唯一保留的自然语言字面量。它不是界面文案（文案全在语言包里），
		// 而是"语言下拉里这一项的显示名"，且仅在 Qt 取不到系统语言名时才用到；
		// 若改成从语言包取，包一旦缺失这一项就会显示成代号，反而更糟。
		if (fallback.name.isEmpty())
			fallback.name = QStringLiteral("中文");
		languages.append(fallback);

		// 候选语言 = qrc 内置包 ∪ 外部 translations/ 里的 .qm。
		// 内置包必须一起算进来：它们是在本次 init() 里刚被释放到外部目录的，
		// 而 Windows 的目录项更新有延迟 —— 同一进程内紧接着 entryList 可能还看不到，
		// 只扫外部目录会导致"首次启动时英文不出现在语言列表里，重启才出现"。
		// 只认 qrc 里确实存在的包（资源是内存数据，没有上面那个时序问题）；
		// parseAvailableLanguages 会去重，重复加入无妨。
		QStringList candidates;
		for (const char* base : kBuiltinPacks) {
			const QString name = QString::fromLatin1(base) + QStringLiteral(".qm");
			if (QFile::exists(QString::fromLatin1(kResourcePrefix) + name))
				candidates.append(name);
		}
		candidates += QDir(translationsDir()).entryList(QDir::Files, QDir::Name);

		const QVector<LanguageInfo> found = parseAvailableLanguages(candidates);

		for (const LanguageInfo& info : found) {
			if (isDefaultLanguage(info.code))
				continue; // 中文变体归入上面那一槽，不单列
			languages.append(info);
		}

		return languages;
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

			// .ts 结构（ID-based）：<message id="topbar_settings"><translation>…</translation></message>
			// 查表主键是 <message> 的 id 属性，"就地换文案"必须按 id 查；
			// 正式 .ts 不维护 <source>，这里也不再解析它。
			QXmlStreamReader xml(&file);
			QString id;     // 当前 <message> 的 id 属性
			const int before = sources.size();

			while (!xml.atEnd()) {
				xml.readNext();

				if (xml.isStartElement()) {
					const QStringView name = xml.name();
					if (name == QLatin1String("message")) {
						id = xml.attributes().value(QLatin1String("id")).toString();
					}
				}
				else if (xml.isEndElement() && xml.name() == QLatin1String("message")) {
					if (!id.isEmpty()) {
						TranslationSource entry;
						entry.id = id;
						sources.append(entry);
					}
					id.clear();
				}
			}

			if (xml.hasError()) {
				qWarning().noquote() << QStringLiteral("[Translation] manifest parse error:")
					<< file.fileName() << xml.errorString();
			}
			else if (sources.size() == before) {
				// 多半是份老式的 tr() 清单（<message> 没有 id 属性）：换文案会整个降级
				qWarning().noquote() << QStringLiteral("[Translation] manifest has no id-based messages:")
					<< file.fileName()
					<< QStringLiteral("-> in-place retranslate will do nothing for it");
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
		releaseBuiltinPack(translationFileBase(defaultLanguageCode()), dir);

		if (isDefaultLanguage(wanted)) {
			// 默认语言（中文）同样依赖语言包：文案只存在于数据文件里，改文案不必重编译。
			// 注意兜底变成了 **key 本身**（qtTrId 对未知 id 原样返回），不再是中文原文，
			// 所以内置包缺失会让界面露出 topbar_settings 这类代号。
			const QString origin = installFrom(translationFileBase(wanted), dir);
			if (origin.isEmpty()) {
				qWarning().noquote() << QStringLiteral("[Translation] default pack for")
					<< wanted << QStringLiteral("unavailable -> UI will show raw ids");
			}
			return;
		}

		const QString fileBase = translationFileBase(wanted);
		const QString origin = installFrom(fileBase, dir);
		if (!origin.isEmpty())
			return;

		// 找不到对应语言包：退回默认语言的语言包（再退回 key），原因写进日志
		qInfo().noquote() << QStringLiteral("[Translation] no language pack for")
			<< wanted << QStringLiteral("under") << dir
			<< QStringLiteral("-> falling back to the default language");
		g_activeCode = defaultLanguageCode();
		uninstall();
		installFrom(translationFileBase(defaultLanguageCode()), dir);
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
		releaseBuiltinPacks(dir);

		// 源语言与其它语言走同一条路径：能装上语言包就用它，装不上就退回代码原文
		const QString origin = installFrom(translationFileBase(wanted), dir);
		const bool switched = !origin.isEmpty();

		if (!switched) {
			qWarning().noquote() << QStringLiteral("[Translation] language pack not found for")
				<< wanted << QStringLiteral("under") << dir
				<< QStringLiteral("-> falling back to the default language");
			uninstall();
			installFrom(translationFileBase(defaultLanguageCode()), dir);
		}

		const QString active = switched ? wanted : defaultLanguageCode();
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