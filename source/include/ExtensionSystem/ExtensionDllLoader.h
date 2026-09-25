#pragma once

// 工具扩展 DLL 的启动装载（纯 header + inline，无链接改动）：env 显式指定（调试用，只加载这一份
// 并跳过自动发现），否则扫描 extensions/ 与 node_modules/ 下每个扩展目录的 regulation.json5 + main.dll。
// DllCaller 内部按扩展名去重，所以重复目录不会重复加载。
//
// ⚠️ 与 ExtensionLoader 不是一回事：那个管 .ext 扩展包的**安装**（解压 → 落盘 → 分流客户端/工具扩展），
//    本文件只管启动时**认出已经装好的**工具扩展并交给 DllCaller。
// ⚠️ 与 ClientExtension::loadAll() 也不是一回事：那个装的是客户端扩展（跑在 GUI 线程、改宿主界面），
//    本文件装的是工具扩展（跑 Worker 线程、只做 JSON 工具调用）。

#include "ExtensionSystem/DllCaller.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace ExtensionDllLoader
{
	// 把已安装的工具扩展交给 caller 装载。caller 由调用方创建并持有，本函数不接管所有权。
	// serverProfilePath = <appDir>/resources/server/harness/profiles/web
	// 任何单个扩展失败都只记日志、不阻断其它扩展（与改造前一致）。
	inline void loadAll(DllCaller* caller, const QString& serverProfilePath)
	{
		if (!caller)
			return;

		QString descriptorPath = qEnvironmentVariable("DSH_DLL_JSON5", QString());
		QString dllPath = qEnvironmentVariable("DSH_DLL", QString());

		// 显式指定了完整一对（调试用）：只加载这一份，跳过自动发现
		if (!descriptorPath.isEmpty() && !dllPath.isEmpty()) {
			if (QFile::exists(descriptorPath) && QFile::exists(dllPath)) {
				if (!caller->loadDescriptor(descriptorPath)) {
					qWarning() << "[DllCaller] descriptor error:" << caller->errorString();
				}
				else if (!caller->loadLibrary(dllPath)) {
					qWarning() << "[DllCaller] library error:" << caller->errorString();
				}
			}
			return;
		}

		// env 只给了描述符（调试）：目录下 main.dll 作为库一并加
		if (!descriptorPath.isEmpty() && QFile::exists(descriptorPath)) {
			if (dllPath.isEmpty())
				dllPath = QFileInfo(descriptorPath).absolutePath() + QStringLiteral("/main.dll");
			if (QFile::exists(dllPath)) {
				if (!caller->loadDescriptor(descriptorPath)) {
					qWarning() << "[DllCaller] descriptor error:" << caller->errorString();
				}
				else if (!caller->loadLibrary(dllPath)) {
					qWarning() << "[DllCaller] library error:" << caller->errorString();
				}
			}
		}

		// 自动发现：逐个加载全部已安装扩展（原先只加载扫描到的第一个；
		// 多扩展并存后改为全部加载，DllCaller 内部按扩展名去重）
		const QStringList scanRoots = {
			serverProfilePath + QStringLiteral("/extensions"),
			serverProfilePath + QStringLiteral("/node_modules")
		};
		for (const QString& scanRoot : scanRoots) {
			const QDir root(scanRoot);
			if (!root.exists())
				continue;
			const QFileInfoList entries =
				root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
			for (const QFileInfo& entry : entries) {
				const QString extDir = entry.absoluteFilePath();
				const QString candidateJson = extDir + QStringLiteral("/regulation.json5");
				const QString candidateDll = extDir + QStringLiteral("/main.dll");
				if (!QFile::exists(candidateJson) || !QFile::exists(candidateDll))
					continue;
				if (!caller->loadDescriptor(candidateJson)) {
					qWarning() << "[DllCaller] auto-load descriptor failed:" << candidateJson
						<< caller->errorString();
					continue;
				}
				if (!caller->loadLibrary(candidateDll)) {
					qWarning() << "[DllCaller] auto-load library failed:" << candidateDll
						<< caller->errorString();
				}
			}
		}
	}
}
