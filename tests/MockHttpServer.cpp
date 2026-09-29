#include "MockHttpServer.h"

#include <QHostAddress>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

namespace {

QByteArray reasonPhrase(int status) {
  switch (status) {
  case 200:
    return QByteArrayLiteral("OK");
  case 302:
    return QByteArrayLiteral("Found");
  case 404:
    return QByteArrayLiteral("Not Found");
  case 500:
    return QByteArrayLiteral("Internal Server Error");
  default:
    return QByteArrayLiteral("Status");
  }
}

QString requestPath(const QByteArray &request) {
  const QList<QByteArray> firstLine =
      request.left(request.indexOf('\n')).split(' ');
  if (firstLine.size() < 2) {
    return QStringLiteral("/");
  }
  return QString::fromLatin1(firstLine.at(1));
}

} // namespace

MockHttpServer::MockHttpServer(QObject *parent) : QTcpServer(parent) {}

MockHttpServer::~MockHttpServer() {
  for (QTcpSocket *socket : std::as_const(m_sockets)) {
    socket->disconnectFromHost();
    socket->deleteLater();
  }
}

bool MockHttpServer::start() { return listen(QHostAddress::LocalHost, 0); }

QUrl MockHttpServer::url(const QString &path) const {
  QUrl result;
  result.setScheme(QStringLiteral("http"));
  result.setHost(QStringLiteral("127.0.0.1"));
  result.setPort(serverPort());
  result.setPath(path);
  return result;
}

void MockHttpServer::setResponse(const QString &path, Response response) {
  m_routes.insert(path, std::move(response));
}

void MockHttpServer::incomingConnection(qintptr socketDescriptor) {
  auto *socket = new QTcpSocket(this);
  if (!socket->setSocketDescriptor(socketDescriptor)) {
    socket->deleteLater();
    return;
  }
  m_sockets.append(socket);
  connect(socket, &QObject::destroyed, this, [this, socket]() {
    m_sockets.removeAll(socket);
    m_buffers.remove(socket);
  });
  connect(socket, &QTcpSocket::readyRead, this,
          [this, socket]() { handleRequest(socket); });
}

void MockHttpServer::handleRequest(QTcpSocket *socket) {
  QByteArray &request = m_buffers[socket];
  request += socket->readAll();
  if (!request.contains("\r\n\r\n")) {
    return;
  }
  m_lastRequest = request;
  const QString path = requestPath(request);
  m_buffers.remove(socket);
  const Response response =
      m_routes.value(path, Response{404, QByteArrayLiteral("text/plain"),
                                    QByteArrayLiteral("missing")});
  if (response.hang) {
    return;
  }
  writeResponse(socket, response);
}

void MockHttpServer::writeResponse(QTcpSocket *socket,
                                   const Response &response) {
  QByteArray header = "HTTP/1.1 " + QByteArray::number(response.status) + ' ' +
                      reasonPhrase(response.status) +
                      "\r\nConnection: close\r\n";
  if (!response.contentType.isEmpty()) {
    header += "Content-Type: " + response.contentType + "\r\n";
  }
  if (!response.location.isEmpty()) {
    header += "Location: " + response.location + "\r\n";
  }
  if (response.includeContentLength) {
    qsizetype length = response.body.size();
    for (const QByteArray &chunk : response.chunks) {
      length += chunk.size();
    }
    header += "Content-Length: " + QByteArray::number(length) + "\r\n";
  }
  header += "\r\n";
  socket->write(header);

  if (response.chunks.isEmpty()) {
    socket->write(response.body);
    socket->disconnectFromHost();
    return;
  }

  QPointer<QTcpSocket> guardedSocket(socket);
  for (qsizetype i = 0; i < response.chunks.size(); ++i) {
    const QByteArray chunk = response.chunks.at(i);
    const bool isLast = i == response.chunks.size() - 1;
    QTimer::singleShot(static_cast<int>(i) * response.chunkDelayMs, this,
                       [guardedSocket, chunk, isLast]() {
                         if (!guardedSocket) {
                           return;
                         }
                         guardedSocket->write(chunk);
                         if (isLast) {
                           guardedSocket->disconnectFromHost();
                         }
                       });
  }
}
