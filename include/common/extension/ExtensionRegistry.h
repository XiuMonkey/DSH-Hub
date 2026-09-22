#pragma once

// 已安装扩展清单与 profile 目录相关文件的维护逻辑（无控件）：读写 <profile>/extensions.json、推导扩展落地路径、删除扩展目录（node_modules/<name> 与 extensions/<name>）、维护 cordis.patch.yml（移除单条、清理指向不存在包的残留）。

#include <QString>
#include <QStringList>

class ExtensionRegistry
{
public:
	explicit ExtensionRegistry(const QString& serverProfilePath = QString());

	QString serverProfilePath() const;
	// <profile>/node_modules
	QString nodeModulesPath() const;
	// <profile>/extensions.json
	QString registryPath() const;
	// <profile>/extensions
	QString extensionsDir() const;

	// 扩展安装后固定落地的路径（避免依赖临时解压目录，重启后也能加载）
	QString extensionJsonPath(const QString& name) const;
	QString extensionDllPath(const QString& name) const;

	QStringList installedExtensions() const;
	bool saveInstalledExtensions(const QStringList& names) const;
	// 登记一个扩展（已存在则不重复写入）；返回是否发生了写入
	bool registerInstalled(const QString& name) const;
	// 从清单中移除一个扩展；返回是否发生了变化
	bool unregisterInstalled(const QString& name) const;

	// 删除扩展目录；失败时把原因写入 error
	bool removeExtensionDirectory(const QString& name, QString* error = nullptr) const;

	// 追加一条插件行的结果：失败 / 已存在（未改动）/ 本次追加
	enum class PatchEntryResult
	{
		Failed,
		AlreadyPresent,
		Added,
	};

	// 移除指定扩展的条目（ExtensionLoader 写入的两行结构）
	static bool removePatchEntry(const QString& profilePath, const QString& name);

	// 写侧的对应物：确保 <profile>/cordis.patch.yml 里有这一行。id 与 name 分开传是因为两者不总相等（如 session-stats 的 id 是短名、name 是包名）；判重看 name 行；comment 非空时写成一行 ASCII 注释（该文件常被别的工具读，无 BOM 的中文注释容易显示成乱码）。
	static PatchEntryResult ensurePatchEntry(const QString& profilePath,
		const QString& id,
		const QString& name,
		const QString& comment = QString(),
		QString* error = nullptr);

	// 清理残留配置的结果
	struct CleanupResult
	{
		bool patchReadable = false; // cordis.patch.yml 是否可读
		QStringList removed;        // 被清理掉的扩展名
	};

	// 扫描 cordis.patch.yml，清理指向不存在包的条目（并同步 extensions.json）
	CleanupResult cleanupResiduals() const;

private:
	QString m_serverProfilePath;
	// cordis.patch.yml 中由 ExtensionLoader 写入的两行
	static QString patchIdLine(const QString& name);
	static QString patchNameLine(const QString& name);
};
