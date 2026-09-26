#pragma once

// 工具扩展 DLL 的启动装载（纯 header + inline）：env 显式指定则只加载那一份，否则扫描 extensions/
// 与 node_modules/ 下每个扩展目录的 regulation.json5 + main.dll；caller 内部按扩展名去重。
// 与 ExtensionLoader（管 .ext 安装）、ClientExtension::loadAll()（客户端扩展）不是一回事。

#include "ExtensionSystem/DllCaller.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace ExtensionDllLoader
{
	// caller 由调用方持有；单个扩展失败只记日志、不阻断其它扩展
	inline void loadAll(DllCaller* caller, const QString& serverProfilePath)
	{
		if (!caller)
			return;

		QString descriptorPath = qEnvironmentVariable("DSH_DLL_JSON5", QString());
		QString dllPath = qEnvironmentVariable("DSH_DLL", QString());

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

		const QStringList scanRoots = {
			serverProfilePath + QStringLiteral("/extensions"),
			serverProfilePath + QStringLiteral("/node_modules")
		};
		for (const QString& scanRoot : scanRoots) {
			const QDir root(scanRoot);
			if (!root.exists())
				continue;
			const QFileInfoList entries = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
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
