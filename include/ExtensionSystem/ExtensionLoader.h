#pragma once

// ------------------------------------------------------------------
// ExtensionLoader.h
// ------------------------------------------------------------------
// .ext 扩展包（zip）的解析与安装，按 regulation.json5 里的 Type 分流：
//   无 Type / 其它值        → 工具扩展：包内有 Function[] 与 AttachedPlugin/，装进
//                             服务端扩展目录（extensions.json + cordis.patch.yml），
//                             由 DllCaller 在 Worker 线程按 JSON 协议调用。
//   ClientExtension[Debug]  → 客户端扩展：包里只需 regulation + 一个 dll，装进
//                             <exe>/clientExtensions/<Name>/，由 ClientExtension 在
//                             GUI 线程用 QPluginLoader 装载。详见 ClientExtension.h。
// 两者只共用 .ext 这层壳：载荷、宿主、线程、登记位置都不同。
//
// 标准 .ext 布局（工具扩展）：
//   AttachedPlugin/{package.json,index.js} + main.dll + regulation.json5
// ------------------------------------------------------------------

#include <QString>

class ExtensionLoader
{
public:
	struct LoadedExtension
	{
		QString jsonPath;   // regulation.json5（客户端扩展：已落到扩展目录的那份）
		QString dllPath;    // 载荷 DLL（客户端扩展：已落到扩展目录的那份）
		QString pluginPath; // AttachedPlugin 目录（只有工具扩展才有）
		QString pluginName; // 扩展名（客户端扩展取自 regulation 的 Name）

		// ---- 由 regulation 解析出来的字段 ----
		QString type;         // Type 字段原文（工具扩展没有这个字段 → 空）
		QString declaredName; // Name 字段原文

		// true = 走了客户端扩展路线：调用方应在 GUI 线程调
		// ClientExtension::loadOne(pluginName, dllPath)，且不要碰
		// extensions.json / cordis.patch.yml / 服务端重启。
		bool isClientExtension = false;
	};

	bool loadAndInstall(const QString& extFilePath,
		const QString& serverProfilePath,
		LoadedExtension* out,
		QString* error = nullptr);

private:
	bool extractArchive(const QString& extFilePath,
		const QString& destDir,
		QString* error);
	bool findFiles(const QString& rootDir,
		LoadedExtension* out,
		QString* error);
	// 读 regulation.json5（JSON5：允许注释与尾随逗号），取出 Name / Type。
	bool readDescriptor(const QString& jsonPath,
		LoadedExtension* out,
		QString* error);
	bool installPlugin(const QString& pluginPath,
		const QString& serverProfilePath,
		LoadedExtension* out,
		QString* error);

	QString m_errorString;
};