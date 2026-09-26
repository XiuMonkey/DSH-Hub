#pragma once

// 插件市场包（dshmarket）缺失时的自动安装逻辑：PATH 里没有 pnpm 时生成 pnpm shim（用本地 node 跑 pnpm.cjs），
// 以 DSH_HOME / PATH 环境变量启动 `pnpm add dshmarket`，装完把 dshmarket 写进 profile 的
// package.json（dependencies + dsh.profile.bundles）。只对外抛“开始 / 输出行 / 结束”三种通知，界面文案由 UI 决定。

#include <QObject>
#include <QString>

class QProcess;

class PluginMarketInstaller : public QObject
{
	Q_OBJECT

public:
	explicit PluginMarketInstaller(QObject* parent = nullptr);

	// 市场包已存在、或安装已在进行中时返回 false（调用方无需做任何事）
	bool ensureInstalled(const QString& appDir);
	bool isRunning() const;

signals:
	void installStarted();
	// pnpm 的一行输出（已 trim，非空）
	void installOutput(const QString& line);
	// 安装结束；ok 为 true 表示退出码 0 且 profile 清单已更新
	void installFinished(bool ok);

private:
	void patchProfileManifest(const QString& profileDir) const;

	QProcess* m_installer = nullptr;
};
