#pragma once
#include <QObject>
#include <QVariant>
#include <QVariantList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QByteArray>

// Hand-rolled minimal HTTP GET listener (QTcpServer, no Qt6::HttpServer) that
// lets an external system (e.g. Home Assistant) trigger playback of a Plex
// item on this already-running instance:
//
//   GET /play?service=plex&ratingKey=<key>&token=<token>
//
// See REMOTE_CONTROL_DESIGN.md for the full protocol/auth/log design.
class RemoteControlBackend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool listening READ listening NOTIFY listeningChanged)
    Q_PROPERTY(int port READ port CONSTANT)
    Q_PROPERTY(QString token READ token CONSTANT)
    // Set when startListening() fails (e.g. the port is already in use),
    // cleared on a successful listen. Lets Root.qml distinguish "the user
    // turned this off" from "enabled, but couldn't bind the port" — both look
    // identical from listening() alone.
    Q_PROPERTY(QString lastError READ lastError NOTIFY listeningChanged)
public:
    explicit RemoteControlBackend(const QString &dataRoot, QObject *parent = nullptr);

    // Most-recent-first, capped at 50 entries. Backs Root.qml's initial paint;
    // live updates arrive via commandReceived.
    Q_INVOKABLE QVariantList getCommandLog() const;

    bool listening() const;
    int port() const { return kPort; }
    QString token() const { return m_token; }
    QString lastError() const { return m_lastError; }

signals:
    // Emitted once per received /play request, whether accepted or rejected.
    void commandReceived(const QString &timestamp, const QString &sourceIp,
                          const QString &service, const QString &ratingKey,
                          bool accepted);
    // Emitted only on an accepted "play plex item" command.
    void playPlexItemRequested(const QString &ratingKey);
    void listeningChanged();

public slots:
    void onSettingChanged(const QString &moduleId, const QString &key, const QVariant &value);

private slots:
    void handleNewConnection();
    void onSocketReadyRead();

private:
    static constexpr int kPort = 8420;
    // Guards against a client that never sends a full request line (slow-loris
    // style) — drop the connection rather than growing the buffer forever.
    static constexpr int kMaxRequestLineBytes = 8192;

    void setListeningEnabled(bool enabled);
    void startListening();
    void stopListening();

    void handleRequestLine(QTcpSocket *socket, const QString &line);
    void sendResponse(QTcpSocket *socket, int statusCode, const QString &status,
                       const QString &message);
    void appendLogEntry(const QString &timestamp, const QString &sourceIp,
                         const QString &service, const QString &ratingKey, bool accepted);

    QString loadOrCreateToken() const;
    static QString generateToken();

    QString m_dataRoot;
    QString m_token;
    QString m_lastError;
    bool m_enabled = false;
    QTcpServer *m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QList<QVariantMap> m_commandLog;
};
