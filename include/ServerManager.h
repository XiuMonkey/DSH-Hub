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

	// 出厂 settings.yaml：本部署**不随附任何模型**（写 `llm-deepseek: models: []`）。
	// 为什么要显式写空数组：适配器自带的默认目录（deepseek-flash 等 4 条）只在
	// 该路由的 `models` 缺席时才生效 —— 写了空数组才等于"这条路由一条也不公布"。
	// 只在文件缺失时写入：用户配置过的 harness 一个字都不动（返回 true 表示可用）。
	// 之所以落在客户端：数据根是客户端建的，缺这一步时"清空 harness"会让随附模型复活。
	static bool ensureFactorySettings(const QString& dshHome);

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
