#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QUrl>

class ServerManager : public QObject
{
    Q_OBJECT

public:
    explicit ServerManager(QObject *parent = nullptr);
    ~ServerManager() override;

    // 启动内置 DSH 服务；如果传入已有 baseUrl/进程，则复用而不是新启动
    void start(const QUrl &initialBaseUrl = QUrl(),
               QProcess *initialServerProcess = nullptr);
    void restart();

    // 主题切换等场景移交/接管服务进程
    QProcess *takeProcess();
    void adoptProcess(QProcess *process);

    QString dshHome() const;
    QUrl baseUrl() const;
    bool isRestarting() const;

signals:
    void baseUrlReady(const QUrl &url);
    void outputLine(const QString &line);
    void errorLine(const QString &line);
    void finished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    void startBundledServer();
    void launchBundledServer(const QString &nodePath,
                             const QString &entryPath,
                             const QString &dshEntry,
                             const QString &cwd,
                             const QString &dshHome,
                             int port = 0);
    void handleServerOutput();
    void handleServerFinished(int exitCode, QProcess::ExitStatus exitStatus);

    QProcess *m_serverProcess = nullptr;
    QString m_dshHome;
    bool m_restarting = false;
    QUrl m_baseUrl;
};
