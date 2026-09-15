#pragma once

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

	// 启动内置 DSH 服务；如果传入已有 baseUrl/进程，则复用而不是新启动
	void start(const QUrl& initialBaseUrl = QUrl(),
		QProcess* initialServerProcess = nullptr);
	void restart();

	// 主题切换等场景移交服务进程（接管方通过 start() 的 initialServerProcess 参数）
	QProcess* takeProcess();

	QString dshHome() const;
	bool isRestarting() const;

signals:
	void baseUrlReady(const QUrl& url);
	void outputLine(const QString& line);
	void errorLine(const QString& line);
	void finished(int exitCode, QProcess::ExitStatus exitStatus);

private:
	void startBundledServer();
	// 内置插件安装：把 qrc 里的插件源写进 profile 的 node_modules 并登记到
	// cordis.patch.yml。台账（.dsh-hub-builtin.json）记录已装内容的 revision，
	// 因此是"首次装一次、源码变了才重装"，不是每次启动都覆盖。
	void ensureBuiltinPlugins(const QString& profileDir);
	// 单个内置插件的落地：qrc → <profile>/node_modules/<name>/ + patch 行。
	// 装任意一个内置插件就是这一次调用（清单里加一行即可）。
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
