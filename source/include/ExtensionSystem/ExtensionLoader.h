#pragma once

// .ext 包（zip）的解析与安装，按 regulation.json5 的 Type 分流。
// ClientExtension[Debug] → 客户端扩展：装进 <exe>/clientExtensions/<Name>/，GUI 线程 QPluginLoader 装载；
// 其余 → 工具扩展：服务端扩展目录，Worker 线程按 JSON 协议调用。

#include <QString>

class ExtensionLoader
{
public:
	struct LoadedExtension
	{
		QString jsonPath;
		QString dllPath;
		QString pluginPath;
		QString pluginName;

		QString type;
		QString declaredName;

		// true = 客户端扩展路线：GUI 线程调 ClientExtension::loadOne，不碰服务端配置
		bool isClientExtension = false;
	};

	bool loadAndInstall(const QString& extFilePath, const QString& serverProfilePath, LoadedExtension* out,
		QString* error = nullptr);

private:
	bool extractArchive(const QString& extFilePath, const QString& destDir, QString* error);
	bool findFiles(const QString& rootDir, LoadedExtension* out, QString* error);
	bool readDescriptor(const QString& jsonPath, LoadedExtension* out, QString* error);
	bool installPlugin(const QString& pluginPath, const QString& serverProfilePath, LoadedExtension* out,
		QString* error);

	QString m_errorString;
};
