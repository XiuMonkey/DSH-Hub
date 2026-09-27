#include "common/settings/SettingsStore.h"

#include "common/settings/ClientSettings.h"

#include <QJsonObject>

namespace
{
	const char* const kServerFile = "ServerSetting.json";
	const char* const kUrlKey = "url";
}

QString SettingsStore::fileName()
{
	return QLatin1String(kServerFile);
}

QString SettingsStore::serverUrl()
{
	return ClientSettings::read(kServerFile)
		.value(QLatin1String(kUrlKey)).toString().trimmed();
}

void SettingsStore::setServerUrl(const QString& url)
{
	QJsonObject object = ClientSettings::read(kServerFile);

	const QString trimmed = url.trimmed();
	if (trimmed.isEmpty())
		object.remove(QLatin1String(kUrlKey));
	else
		object.insert(QLatin1String(kUrlKey), trimmed);

	ClientSettings::write(kServerFile, object);
}
