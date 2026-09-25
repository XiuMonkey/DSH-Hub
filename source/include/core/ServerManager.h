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

	// ---- 后端接管（客户端扩展接管 DSH API 时用，见 core/DshApiClient.h）----
	// 停掉已启动的内置 DSH 进程，并把进程级接管标记置上。⚠️ 幂等：进程不存在、已经退出、
	// 已经停过都算成功（切主题会让同一个插件实例再接管一次，重复到来是常态）。
	void stopForTakeover();

	// 拨动"本进程的后端已被接管"标记。**进程级**（文件作用域静态）而不是实例成员：
	// ServerManager 每个窗口一份，切主题会新建，而接管状态跨窗口存活 —— 用实例成员会在
	// 切主题后丢掉，新窗口的 start() 又会把 DSH 进程拉回来。
	// 接管态下 start() / restart() 一律 no-op（唯一例外：start() 仍会填充 dshHome）。
	static void setTakenover(bool takenover);
	static bool isTakenover();

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
