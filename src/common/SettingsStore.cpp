#include "SettingsStore.h"

#include <QSettings>

namespace
{
	const char* const kServerUrlKey = "server/url";
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