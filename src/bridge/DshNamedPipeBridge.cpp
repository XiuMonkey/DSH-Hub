// ------------------------------------------------------------------
// DshNamedPipeBridge.cpp
// ------------------------------------------------------------------
// 命名管道桥接服务实现。
// 协议：每个请求/响应都是单行 JSON，以 '\n' 结尾。
//   请求: {"id":1,"tool":"dll_power_func","args":{...}}
//   响应: {"id":1,"ok":true,"result":{...}}
//         {"id":1,"ok":false,"error":"..."}
// ------------------------------------------------------------------

#include "DshNamedPipeBridge.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QDebug>

DshNamedPipeBridge::DshNamedPipeBridge(QObject *parent)
    : QObject(parent)
    , m_server(new QLocalServer(this))
{
    connect(m_server, &QLocalServer::newConnection,
            this, &DshNamedPipeBridge::onNewConnection);
}

bool DshNamedPipeBridge::start(const QString &pipeName)
{
    stop();

    m_pipeName = pipeName;

    // 移除可能残留的旧管道（上次异常退出可能没清干净）
    QLocalServer::removeServer(m_pipeName);

    if (!m_server->listen(m_pipeName)) {
        m_errorString = m_server->errorString();
        qWarning() << "[DshNamedPipeBridge] listen failed:"
                   << m_pipeName << m_errorString;
        return false;
    }

    m_errorString.clear();
    qInfo().noquote() << QStringLiteral("[DSH Pipe] named pipe listening: \\\\.\\pipe\\%1").arg(m_pipeName);
    return true;
}

void DshNamedPipeBridge::stop()
{
    if (m_server && m_server->isListening()) {
        m_server->close();
    }
    for (auto it = m_buffers.begin(); it != m_buffers.end(); ++it) {
        it.key()->deleteLater();
    }
    m_buffers.clear();
}

bool DshNamedPipeBridge::isListening() const
{
    return m_server && m_server->isListening();
}

QString DshNamedPipeBridge::errorString() const
{
    return m_errorString;
}

QString DshNamedPipeBridge::pipeName() const
{
    return m_pipeName;
}

void DshNamedPipeBridge::sendResponse(QLocalSocket *socket,
                                      int id,
                                      bool ok,
                                      const QJsonObject &result,
                                      const QString &error)
{
    if (!socket) {
        return;
    }

    QJsonObject response;
    response.insert(QStringLiteral("id"), id);
    response.insert(QStringLiteral("ok"), ok);
    response.insert(QStringLiteral("result"), result);
    if (!ok && !error.isEmpty()) {
        response.insert(QStringLiteral("error"), error);
    }

    QByteArray bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
    bytes.append('\n');

    socket->write(bytes);
    socket->flush();
        qInfo().noquote() << "[DSH Pipe] response id=" << id << " ok=" << ok;
}

void DshNamedPipeBridge::onNewConnection()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        connect(socket, &QLocalSocket::readyRead,
                this, &DshNamedPipeBridge::onReadyRead);
        connect(socket, &QLocalSocket::disconnected,
                this, &DshNamedPipeBridge::onDisconnected);
        m_buffers.insert(socket, QByteArray());
        emit clientConnected();
        qInfo().noquote() << QStringLiteral("[DSH Pipe] client connected");
    }
}

void DshNamedPipeBridge::onReadyRead()
{
    QLocalSocket *socket = qobject_cast<QLocalSocket *>(sender());
    if (!socket) {
        return;
    }

    QByteArray &buffer = m_buffers[socket];
    buffer.append(socket->readAll());

    // 按换行拆出一个一个完整的 JSON
    int newlineIndex;
    while ((newlineIndex = buffer.indexOf('\n')) != -1) {
        QByteArray line = buffer.left(newlineIndex).trimmed();
        buffer.remove(0, newlineIndex + 1);
        if (!line.isEmpty()) {
            handleLine(socket, line);
        }
    }
}

void DshNamedPipeBridge::onDisconnected()
{
    QLocalSocket *socket = qobject_cast<QLocalSocket *>(sender());
    if (!socket) {
        return;
    }

    m_buffers.remove(socket);
    socket->deleteLater();
    emit clientDisconnected();
    qInfo().noquote() << QStringLiteral("[DSH Pipe] client disconnected");
}

void DshNamedPipeBridge::handleLine(QLocalSocket *socket, const QByteArray &line)
{
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << QStringLiteral("[DSH Pipe] invalid JSON request");
        sendResponse(socket, -1, false, QJsonObject(),
                     QStringLiteral("invalid JSON request"));
        return;
    }

    const QJsonObject request = doc.object();
    const int id = request.value(QStringLiteral("id")).toInt(-1);
    const QString tool = request.value(QStringLiteral("tool")).toString();
    const QJsonObject args = request.value(QStringLiteral("args")).toObject();

    if (tool.isEmpty()) {
        sendResponse(socket, id, false, QJsonObject(),
                     QStringLiteral("missing 'tool' field"));
        return;
    }

    qInfo().noquote() << QStringLiteral("[DSH Pipe] request id=%1 tool=%2").arg(id).arg(tool);
    emit requestReceived(id, tool, args, socket);
}
