#include "common/settings/ClientSettings.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>

namespace
{
	const char* const kDirName = "ClientSetting";
	const char* const kDirEnvVar = "DSHHUB_CLIENT_SETTING_DIR";

	const char* const kAppearanceFile = "AppearanceSetting.json";

	const char* const kLanguageKey = "language";
	const char* const kThemeKey = "theme";

	const char* const kSystemValue = "system";
	const char* const kLightValue = "light";
	const char* const kDarkValue = "dark";
}

QString ClientSettings::dir()
{
	// 覆盖目录（供测试与用户挪位）；每次现读不做缓存
	const QByteArray override = qgetenv(kDirEnvVar);
	const QString trimmed = QString::fromLocal8Bit(override).trimmed();
	if (!trimmed.isEmpty())
		return trimmed;

	return QCoreApplication::applicationDirPath()
		+ QLatin1Char('/') + QLatin1String(kDirName);
}

QString ClientSettings::filePath(const QString& fileName)
{
	return dir() + QLatin1Char('/') + fileName;
}

QJsonObject ClientSettings::read(const QString& fileName)
{
	const QString path = filePath(fileName);

	QFile file(path);
	if (!file.exists())
		return {};

	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		qWarning().noquote() << QStringLiteral("[ClientSettings] cannot read:")
			<< path << file.errorString() << QStringLiteral("-> using defaults");
		return {};
	}

	const QByteArray raw = file.readAll();
	file.close();

	QJsonParseError error{};
	const QJsonDocument doc = QJsonDocument::fromJson(raw, &error);
	if (error.error != QJsonParseError::NoError) {
		qWarning().noquote() << QStringLiteral("[ClientSettings] malformed JSON:")
			<< path << QStringLiteral("at offset") << error.offset
			<< error.errorString() << QStringLiteral("-> using defaults");
		return {};
	}
	if (!doc.isObject()) {
		// 合法 JSON 但根不是对象：按没有处理，别让后面的 .value() 落空
		qWarning().noquote() << QStringLiteral("[ClientSettings] root is not an object:")
			<< path << QStringLiteral("-> using defaults");
		return {};
	}

	return doc.object();
}

bool ClientSettings::write(const QString& fileName, const QJsonObject& object)
{
	const QString path = filePath(fileName);

	const QString folder = QFileInfo(path).absolutePath();
	if (!QDir(folder).exists() && !QDir().mkpath(folder)) {
		qWarning().noquote() << QStringLiteral("[ClientSettings] cannot create dir:")
			<< folder;
		return false;
	}

	// QSaveFile：commit() 时原子替换，中途失败不留半截 JSON
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		qWarning().noquote() << QStringLiteral("[ClientSettings] cannot write:")
			<< path << file.errorString();
		return false;
	}

	file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));

	if (!file.commit()) {
		qWarning().noquote() << QStringLiteral("[ClientSettings] commit failed:")
			<< path << file.errorString();
		return false;
	}

	return true;
}

QString AppearanceSetting::fileName()
{
	return QLatin1String(kAppearanceFile);
}

void AppearanceSetting::ensureFile()
{
	const QString path = ClientSettings::filePath(kAppearanceFile);
	if (QFile::exists(path))
		return;

	QJsonObject object;
	object.insert(QLatin1String(kLanguageKey), QLatin1String(kSystemValue));
	object.insert(QLatin1String(kThemeKey), QLatin1String(kSystemValue));

	if (ClientSettings::write(kAppearanceFile, object))
		qInfo().noquote() << QStringLiteral("[Appearance] created default settings:") << path;
}

QString AppearanceSetting::languageCode()
{
	const QString raw = ClientSettings::read(kAppearanceFile)
		.value(QLatin1String(kLanguageKey)).toString().trimmed();

	// 文件里写 "system"，代码里统一用空串表示"跟随系统"
	if (raw.isEmpty() || raw.compare(QLatin1String(kSystemValue), Qt::CaseInsensitive) == 0)
		return QString();

	return raw;
}

void AppearanceSetting::setLanguageCode(const QString& code)
{
	QJsonObject object = ClientSettings::read(kAppearanceFile);

	const QString trimmed = code.trimmed();
	object.insert(QLatin1String(kLanguageKey), trimmed.isEmpty() ? QString::fromLatin1(kSystemValue) : trimmed);

	ClientSettings::write(kAppearanceFile, object);
}

AppearanceSetting::ThemeMode AppearanceSetting::themeMode()
{
	return themeModeFromName(ClientSettings::read(kAppearanceFile).value(QLatin1String(kThemeKey)).toString());
}

void AppearanceSetting::setThemeMode(ThemeMode mode)
{
	QJsonObject object = ClientSettings::read(kAppearanceFile);
	object.insert(QLatin1String(kThemeKey), themeModeName(mode));
	ClientSettings::write(kAppearanceFile, object);
}

QString AppearanceSetting::themeModeName(ThemeMode mode)
{
	switch (mode) {
	case ThemeMode::Light:
		return QString::fromLatin1(kLightValue);
	case ThemeMode::Dark:
		return QString::fromLatin1(kDarkValue);
	case ThemeMode::System:
	default:
		return QString::fromLatin1(kSystemValue);
	}
}

AppearanceSetting::ThemeMode AppearanceSetting::themeModeFromName(const QString& name)
{
	const QString trimmed = name.trimmed();

	if (trimmed.compare(QLatin1String(kLightValue), Qt::CaseInsensitive) == 0)
		return ThemeMode::Light;
	if (trimmed.compare(QLatin1String(kDarkValue), Qt::CaseInsensitive) == 0)
		return ThemeMode::Dark;

	// 空串、拼错的值一律当跟随系统
	return ThemeMode::System;
}
