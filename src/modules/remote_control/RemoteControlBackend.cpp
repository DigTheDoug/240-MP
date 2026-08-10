#include "RemoteControlBackend.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QUrl>
#include <QUrlQuery>
#include <QRandomGenerator>
#include <QDebug>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

RemoteControlBackend::RemoteControlBackend(const QString &dataRoot, QObject *parent)
    : QObject(parent)
    , m_dataRoot(dataRoot)
    , m_server(new QTcpServer(this))
{
    m_token = loadOrCreateToken();

    connect(m_server, &QTcpServer::newConnection, this, &RemoteControlBackend::handleNewConnection);

    // Same rule as AppCore::isModuleEnabled / the NFC reader module: an
    // unwritten setting means the manifest default (OFF), a present value
    // means whatever it says.
    bool configuredEnabled = false;
    QFile f(m_dataRoot + "/config.json");
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject moduleConfig = QJsonDocument::fromJson(f.readAll()).object()
            ["modules"].toObject()["com.240mp.remote_control"].toObject();
        if (moduleConfig.contains("enabled"))
            configuredEnabled = moduleConfig["enabled"].toBool(true);
    }

    m_enabled = configuredEnabled;
    if (m_enabled)
        startListening();
    else
        qInfo("[RemoteControl] Listener disabled by configuration");
}

// ---------------------------------------------------------------------------
// Token
// ---------------------------------------------------------------------------

QString RemoteControlBackend::loadOrCreateToken() const {
    QFile f(m_dataRoot + "/remote_control_token.json");
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError err;
        const auto doc = QJsonDocument::fromJson(f.readAll(), &err);
        f.close();
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            const QString existing = doc.object()["token"].toString();
            if (!existing.isEmpty())
                return existing;
        }
    }

    const QString generated = generateToken();
    QJsonObject obj;
    obj["token"] = generated;

    QFile out(m_dataRoot + "/remote_control_token.json");
    if (out.open(QIODevice::WriteOnly)) {
        out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        out.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        out.close();
    } else {
        qWarning("[RemoteControl] Could not write remote_control_token.json: %s",
                 qPrintable(out.errorString()));
    }
    return generated;
}

QString RemoteControlBackend::generateToken() {
    // 8 hex chars (32 bits) rather than a full 128-bit token: this is read off
    // a TV screen and hand-typed into a Home Assistant config on another
    // device, so easy transcription matters more than resisting brute force —
    // the listener is LAN-only and every attempt is visible in the command
    // log regardless.
    static const char kHex[] = "0123456789abcdef";
    static constexpr int kTokenLength = 8;
    QString token;
    token.reserve(kTokenLength);
    for (int i = 0; i < kTokenLength; ++i)
        token.append(QLatin1Char(kHex[QRandomGenerator::global()->bounded(16)]));
    return token;
}

// ---------------------------------------------------------------------------
// Listener lifecycle
// ---------------------------------------------------------------------------

bool RemoteControlBackend::listening() const {
    return m_server->isListening();
}

void RemoteControlBackend::setListeningEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (enabled)
        startListening();
    else
        stopListening();
}

void RemoteControlBackend::startListening() {
    if (m_server->isListening()) return;

    if (!m_server->listen(QHostAddress::Any, kPort)) {
        m_lastError = m_server->errorString();
        qWarning("[RemoteControl] Failed to listen on port %d: %s",
                 kPort, qPrintable(m_lastError));
        emit listeningChanged();
        return;
    }
    m_lastError.clear();
    qInfo("[RemoteControl] Listening on port %d", kPort);
    emit listeningChanged();
}

void RemoteControlBackend::stopListening() {
    if (!m_server->isListening()) return;

    // close() drops the listening socket outright (not just ignoring new
    // requests) — already-accepted sockets finish independently and clean
    // themselves up via their disconnected handler.
    m_server->close();
    m_lastError.clear();
    qInfo("[RemoteControl] Stopped listening");
    emit listeningChanged();
}

void RemoteControlBackend::onSettingChanged(const QString &moduleId, const QString &key,
                                             const QVariant &value) {
    if (moduleId != QLatin1String("com.240mp.remote_control")) return;

    if (key == QLatin1String("enabled")) {
        // Same rule as the constructor: only an explicit false turns the
        // listener off.
        const bool enabled = value.metaType().id() != QMetaType::Bool || value.toBool();
        setListeningEnabled(enabled);
    }
}

// ---------------------------------------------------------------------------
// Connection handling
// ---------------------------------------------------------------------------

void RemoteControlBackend::handleNewConnection() {
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, &RemoteControlBackend::onSocketReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_buffers.remove(socket);
            socket->deleteLater();
        });
    }
}

void RemoteControlBackend::onSocketReadyRead() {
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket) return;

    QByteArray &buf = m_buffers[socket];
    buf.append(socket->readAll());

    // We only need the request line (this is a GET-only, body-less protocol),
    // so the rest of the headers — if any — are left unread and discarded
    // when the socket closes.
    const int lineEnd = buf.indexOf("\r\n");
    if (lineEnd < 0) {
        if (buf.size() > kMaxRequestLineBytes) {
            m_buffers.remove(socket);
            socket->disconnectFromHost();
        }
        return;
    }

    const QString requestLine = QString::fromUtf8(buf.left(lineEnd));
    m_buffers.remove(socket);
    handleRequestLine(socket, requestLine);
}

void RemoteControlBackend::handleRequestLine(QTcpSocket *socket, const QString &line) {
    const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 2 || parts[0] != QLatin1String("GET")) {
        sendResponse(socket, 400, "error", "malformed request");
        return;
    }

    const QUrl url(parts[1]);
    if (url.path() != QLatin1String("/play")) {
        sendResponse(socket, 400, "error", "unknown path");
        return;
    }

    const QUrlQuery query(url);
    const QString service   = query.queryItemValue("service", QUrl::FullyDecoded);
    const QString ratingKey = query.queryItemValue("ratingKey", QUrl::FullyDecoded);
    const QString token     = query.queryItemValue("token", QUrl::FullyDecoded);

    const QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    const QString sourceIp  = socket->peerAddress().toString();

    // Token check first: an attacker probing the endpoint shouldn't be able
    // to distinguish "bad token" from "bad params" by response shape alone
    // mattering, and it keeps the log's accepted/rejected reason unambiguous.
    if (token != m_token) {
        appendLogEntry(timestamp, sourceIp, service, ratingKey, false);
        sendResponse(socket, 403, "error", "bad token");
        return;
    }

    if (service != QLatin1String("plex") || ratingKey.isEmpty()) {
        appendLogEntry(timestamp, sourceIp, service, ratingKey, false);
        const QString message = service.isEmpty() ? "missing service"
                               : ratingKey.isEmpty() && service == QLatin1String("plex") ? "missing ratingKey"
                               : "unknown service";
        sendResponse(socket, 400, "error", message);
        return;
    }

    appendLogEntry(timestamp, sourceIp, service, ratingKey, true);
    sendResponse(socket, 200, "ok", QString());
    emit playPlexItemRequested(ratingKey);
}

void RemoteControlBackend::sendResponse(QTcpSocket *socket, int statusCode, const QString &status,
                                         const QString &message) {
    QJsonObject body;
    body["status"] = status;
    if (!message.isEmpty())
        body["message"] = message;
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QByteArray statusLine;
    switch (statusCode) {
        case 200:  statusLine = "200 OK"; break;
        case 403:  statusLine = "403 Forbidden"; break;
        default:   statusLine = "400 Bad Request"; break;
    }

    const QByteArray response = "HTTP/1.1 " + statusLine + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + QByteArray::number(json.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + json;

    socket->write(response);
    socket->disconnectFromHost();
}

// ---------------------------------------------------------------------------
// Command log
// ---------------------------------------------------------------------------

void RemoteControlBackend::appendLogEntry(const QString &timestamp, const QString &sourceIp,
                                           const QString &service, const QString &ratingKey,
                                           bool accepted) {
    QVariantMap entry;
    entry["timestamp"] = timestamp;
    entry["sourceIp"] = sourceIp;
    entry["service"] = service;
    entry["ratingKey"] = ratingKey;
    entry["accepted"] = accepted;

    m_commandLog.prepend(entry);
    while (m_commandLog.size() > 50)
        m_commandLog.removeLast();

    emit commandReceived(timestamp, sourceIp, service, ratingKey, accepted);
}

QVariantList RemoteControlBackend::getCommandLog() const {
    QVariantList list;
    list.reserve(m_commandLog.size());
    for (const auto &entry : m_commandLog)
        list.append(entry);
    return list;
}
