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

	// 传入已有 baseUrl/进程则复用
	void start(const QUrl& initialBaseUrl = QUrl(), QProcess* initialServerProcess = nullptr);
	void restart();

	// 后端接管：停掉内置进程并置接管标记；幂等，重复接管是常态
	void stopForTakeover();

	// 接管态跨窗口存活，故标记做进程级静态；接管态下 start()/restart() 为 no-op
	static void setTakenover(bool takenover);
	static bool isTakenover();

	QProcess* takeProcess();

	QString dshHome() const;
	bool isRestarting() const;

	// 内置服务端进程的工作目录，也是它的默认工作区路径（`<exe>/resources/server/launch-root`）。
	// 服务端启动时以它为 cwd，会话不带 workspaceId 时也落到这里；客户端要补建"默认工作区"时必须用同一个路径。
	QString defaultWorkspacePath() const;

	// 出厂 settings.yaml：只在缺失时写；必须写空 models: []，缺该键适配器会退回自带目录
	static bool ensureFactorySettings(const QString& dshHome);

signals:
	void baseUrlReady(const QUrl& url);
	void outputLine(const QString& line);
	void errorLine(const QString& line);
	void finished(int exitCode, QProcess::ExitStatus exitStatus);

private:
	void startBundledServer();
	// m_restarting 须先于 emit 落地，否则重启后界面一直转圈
	void publishBaseUrl(const QUrl& url);
	// 台账记 revision：只首次装，源码变了才重装
	void ensureBuiltinPlugins(const QString& profileDir);
	bool installBuiltinPlugin(const QString& profileDir, const QString& pluginName, QString* error);
	void launchBundledServer(const QString& nodePath, const QString& entryPath, const QString& dshEntry,
		const QString& cwd, const QString& dshHome);
	void handleServerOutput();
	// 服务端致命错误大多打 stderr；不接这条，启动失败时客户端毫无感知，初始化遮罩会一直挂着
	void handleServerErrorOutput();
	void handleServerFinished(int exitCode, QProcess::ExitStatus exitStatus);

	QProcess* m_serverProcess = nullptr;
	QString m_dshHome;
	bool m_restarting = false;
	QUrl m_baseUrl;
};
