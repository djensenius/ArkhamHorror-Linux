#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QTcpServer>
#include <QUrl>
#include <QVector>

class QTcpSocket;

class MockHttpServer final : public QTcpServer {
public:
  struct Response {
    int status{200};
    QByteArray contentType{QByteArrayLiteral("image/png")};
    QByteArray body;
    QByteArray location;
    bool includeContentLength{true};
    qint64 contentLengthOverride{-1};
    QVector<QByteArray> chunks;
    int headerDelayMs{0};
    int chunkDelayMs{10};
    bool hang{false};
    bool hangAfterHeaders{false};
  };

  explicit MockHttpServer(QObject *parent = nullptr);
  ~MockHttpServer() override;

  bool start();
  QUrl url(const QString &path) const;
  void setResponse(const QString &path, Response response);
  QByteArray lastRequest() const { return m_lastRequest; }
  int disconnectCount() const { return m_disconnectCount; }

protected:
  void incomingConnection(qintptr socketDescriptor) override;

private:
  void handleRequest(QTcpSocket *socket);
  void writeResponse(QTcpSocket *socket, const Response &response);

  QHash<QString, Response> m_routes;
  QList<QTcpSocket *> m_sockets;
  QHash<QTcpSocket *, QByteArray> m_buffers;
  QByteArray m_lastRequest;
  int m_disconnectCount{0};
};
