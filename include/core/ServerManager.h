#pragma once

// 内置 DSH 服务端进程的生命周期：拉起 / 重启 / 移交给下一个窗口。

#include <QObject>
#include <QProcess>
#include <QString>
#include <QUrl>

class ServerManager : public QObject
{
	Q_OBJECT

public:
	explicit ServerManager(QObject* parent = nullptr);
	~ServerManager() override;

	// 启动内置 DSH 服务；传入已有 baseUrl/进程则复用而不是新启动
	void start(const QUrl& initialBaseUrl = QUrl(),
		QProcess* initialServerProcess = nullptr);
	void restart();

	// 移交服务进程（接管方通过 start() 的 initialServerProcess 参数拿回）
	QProcess* takeProcess();

	QString dshHome() const;
	bool isRestarting() const;

	// 出厂 settings.yaml：只在文件缺失时写（返回是否已有可用的）；必须显式写空 `models: []`，该键缺席时适配器会退回自带默认目录。
	static bool ensureFactorySettings(const QString& dshHome);

signals:
	void baseUrlReady(const QUrl& url);
	void outputLine(const QString& line);
	void errorLine(const QString& line);
	void finished(int exitCode, QProcess::ExitStatus exitStatus);

private:
	void startBundledServer();
	// 发布 baseUrl。⚠️ m_restarting 必须先于 emit 落地，否则界面会在重启后一直转圈。
	void publishBaseUrl(const QUrl& url);
	// 内置插件安装：qrc 源写进 profile 的 node_modules 并登记 cordis.patch.yml；台账记 revision，故只首次装、源码变了才重装。
	void ensureBuiltinPlugins(const QString& profileDir);
	// 单个内置插件的落地：qrc → <profile>/node_modules/<name>/ + patch 行。
	bool installBuiltinPlugin(const QString& profileDir,
		const QString& pluginName,
		QString* error);
	void launchBundledServer(const QString& nodePath,
		const QString& entryPath,
		const QString& dshEntry,
		const QString& cwd,
		const QString& dshHome,
		int port = 0);
	void handleServerOutput();
	void handleServerFinished(int exitCode, QProcess::ExitStatus exitStatus);

	QProcess* m_serverProcess = nullptr;
	QString m_dshHome;
	bool m_restarting = false;
	QUrl m_baseUrl;
};
