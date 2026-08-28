#pragma once

// ------------------------------------------------------------------
// ExtensionLoader.h
// ------------------------------------------------------------------
// Standard .ext layout:
//   AttachedPlugin/
//       package.json
//       index.js
//   main.dll
//   regulation.json5
// ------------------------------------------------------------------

#include <QString>

class ExtensionLoader
{
public:
    struct LoadedExtension
    {
        QString rootDir;
        QString jsonPath;
        QString dllPath;
        QString pluginPath;
        QString pluginName;
    };

    bool loadAndInstall(const QString &extFilePath,
                        const QString &serverProfilePath,
                        LoadedExtension *out,
                        QString *error = nullptr);

    QString errorString() const;

private:
    bool extractArchive(const QString &extFilePath,
                        const QString &destDir,
                        QString *error);
    bool findFiles(const QString &rootDir,
                   LoadedExtension *out,
                   QString *error);
    bool installPlugin(const QString &pluginPath,
                       const QString &serverProfilePath,
                       LoadedExtension *out,
                       QString *error);

    QString m_errorString;
};