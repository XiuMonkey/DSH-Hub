#include "SettingsStore.h"

#include <QSettings>

namespace
{
	const char* const kServerUrlKey = "server/url";
	const char* const kAgentPresetKey = "agent/defaultPreset";
	const char* const kLanguageKey = "ui/language";
}

QString SettingsStore::serverUrl()
{
	QSettings settings;
	return settings.value(QLatin1String(kServerUrlKey)).toString().trimmed();
}

void SettingsStore::setServerUrl(const QString& url)
{
	QSettings settings;
	const QString trimmed = url.trimmed();
	if (trimmed.isEmpty())
		settings.remove(QLatin1String(kServerUrlKey));
	else
		settings.setValue(QLatin1String(kServerUrlKey), trimmed);
}

QString SettingsStore::defaultAgentPresetId()
{
	QSettings settings;
	return settings.value(QLatin1String(kAgentPresetKey)).toString();
}

void SettingsStore::setDefaultAgentPresetId(const QString& presetId)
{
	QSettings settings;
	settings.setValue(QLatin1String(kAgentPresetKey), presetId);
}

QString SettingsStore::loadLanguageCode()
{
	QSettings settings;
	return settings.value(QLatin1String(kLanguageKey)).toString().trimmed();
}

void SettingsStore::saveLanguageCode(const QString& code)
{
	QSettings settings;
	const QString trimmed = code.trimmed();
	if (trimmed.isEmpty())
		settings.remove(QLatin1String(kLanguageKey)); // 空 = 跟随系统
	else
		settings.setValue(QLatin1String(kLanguageKey), trimmed);
}