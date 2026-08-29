#include "MockHttpServer.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <memory>

MockHttpServer::MockHttpServer(QObject *parent)
    : QObject(parent), m_server(new QTcpServer(this)) {
  connect(m_server, &QTcpServer::newConnection, this,
          &MockHttpServer::onNewConnection);
  const bool listening = m_server->listen(QHostAddress::LocalHost, 0);
  Q_ASSERT(listening);
  Q_UNUSED(listening);
}

MockHttpServer::~MockHttpServer() = default;

quint16 MockHttpServer::port() const { return m_server->serverPort(); }

QUrl MockHttpServer::baseUrlFor(const QString &path) const {
  QUrl url;
  url.setScheme(QStringLiteral("http"));
  url.setHost(QStringLiteral("127.0.0.1"));
  url.setPort(port());
  url.setPath(path.startsWith(u'/') ? path : u'/' + path);
  return url;
}

void MockHttpServer::setResponse(const QString &path, Response response) {
  m_responses.insert(path, std::move(response));
}

int MockHttpServer::requestCount(const QString &path) const {
  return m_requestCounts.value(path, 0);
}

QHash<QByteArray, QByteArray>
MockHttpServer::lastRequestHeaders(const QString &path) const {
  return m_lastRequestHeaders.value(path);
}

bool MockHttpServer::anyRequestEverHadHeader(
    const QByteArray &lowerHeaderName) const {
  for (const auto &headers : m_lastRequestHeaders) {
    if (headers.contains(lowerHeaderName)) {
      return true;
    }
  }
  return false;
}

qint64 MockHttpServer::lastBytesWrittenForSlowDrip(const QString &path) const {
  return m_lastSlowDripBytesWritten.value(path, -1);
}

void MockHttpServer::onNewConnection() {
  while (QTcpSocket *socket = m_server->nextPendingConnection()) {
    Connection connection;
    connection.socket = socket;
    m_connections.insert(socket, connection);

    connect(socket, &QTcpSocket::readyRead, this,
            [this, socket]() { onReadyRead(socket); });
    connect(socket, &QTcpSocket::disconnected, this,
            [this, socket]() { onDisconnected(socket); });
  }
}

void MockHttpServer::onReadyRead(QTcpSocket *socket) {
  auto it = m_connections.find(socket);
  if (it == m_connections.end()) {
    return;
  }
  Connection &connection = it.value();
  if (connection.headersComplete) {
    return; // any request body (never sent by this fetcher) is ignored
  }
  connection.buffer.append(socket->readAll());
  tryParseAndRespond(connection);
}

void MockHttpServer::onDisconnected(QTcpSocket *socket) {
  m_connections.remove(socket);
  socket->deleteLater();
}

void MockHttpServer::tryParseAndRespond(Connection &connection) {
  const int headerEnd = connection.buffer.indexOf("\r\n\r\n");
  if (headerEnd < 0) {
    return; // headers not fully received yet
  }
  connection.headersComplete = true;

  const QByteArray headerBlock = connection.buffer.left(headerEnd);
  const QList<QByteArray> lines = headerBlock.split('\n');
  if (lines.isEmpty()) {
    connection.socket->disconnectFromHost();
    return;
  }

  // Request line: "GET /path HTTP/1.1"
  const QByteArray requestLine = lines.first().trimmed();
  const QList<QByteArray> requestParts = requestLine.split(' ');
  QString path = QStringLiteral("/");
  if (requestParts.size() >= 2) {
    path = QString::fromUtf8(requestParts.at(1));
  }
  connection.path = path;

  for (int i = 1; i < lines.size(); ++i) {
    const QByteArray line = lines.at(i).trimmed();
    const int colon = line.indexOf(':');
    if (colon <= 0) {
      continue;
    }
    const QByteArray name = line.left(colon).trimmed().toLower();
    const QByteArray value = line.mid(colon + 1).trimmed();
    connection.headers.insert(name, value);
  }

  m_requestCounts[path] = m_requestCounts.value(path, 0) + 1;
  m_lastRequestHeaders[path] = connection.headers;
  emit requestHandled(path);

  const Response response = m_responses.value(path, Response{});
  writeResponse(connection.socket, path, response, connection.headers);
}

void MockHttpServer::writeResponse(
    QTcpSocket *socket, const QString &path, const Response &response,
    const QHash<QByteArray, QByteArray> &requestHeaders) {
  // Conditional-match check: an exact If-None-Match match against the
  // configured ETag, or (if no ETag is configured) any If-Modified-Since
  // presence when a lastModifiedForConditionalMatch is configured,
  // answers with a bodyless 304 instead of the full response.
  const bool etagMatches =
      !response.etagForConditionalMatch.isEmpty() &&
      requestHeaders.value("if-none-match") == response.etagForConditionalMatch;
  const bool lastModifiedMatches =
      !response.lastModifiedForConditionalMatch.isEmpty() &&
      requestHeaders.contains("if-modified-since");

  if (etagMatches || lastModifiedMatches) {
    QByteArray out = "HTTP/1.1 304 Not Modified\r\n";
    if (!response.etagForConditionalMatch.isEmpty()) {
      out += "ETag: " + response.etagForConditionalMatch + "\r\n";
    }
    out += "Content-Length: 0\r\n";
    out += "Connection: close\r\n\r\n";
    socket->write(out);
    socket->disconnectFromHost();
    return;
  }

  QByteArray statusLine = "HTTP/1.1 " + QByteArray::number(response.status) +
                          " " + response.reasonPhrase + "\r\n";
  QByteArray headerBytes = statusLine;
  if (!response.contentType.isEmpty()) {
    headerBytes += "Content-Type: " + response.contentType + "\r\n";
  }
  for (const auto &header : response.extraHeaders) {
    headerBytes += header.first + ": " + header.second + "\r\n";
  }
  headerBytes +=
      "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
  headerBytes += "Connection: close\r\n\r\n";
  socket->write(headerBytes);

  if (response.resetAfterHeaders) {
    socket->disconnectFromHost();
    return;
  }

  if (response.slowDrip) {
    writeSlowDrip(socket, path, response.body, response.chunkSize,
                  response.chunkDelayMs);
    return;
  }

  socket->write(response.body);
  m_lastSlowDripBytesWritten[path] = response.body.size();
  socket->disconnectFromHost();
}

void MockHttpServer::writeSlowDrip(QTcpSocket *socket, const QString &path,
                                   QByteArray body, int chunkSize,
                                   int chunkDelayMs) {
  auto offset = std::make_shared<qint64>(0);
  auto timer = std::make_shared<QTimer>();
  timer->setInterval(chunkDelayMs);

  connect(timer.get(), &QTimer::timeout, this,
          [this, socket, path, body, offset, chunkSize, timer]() mutable {
            if (!m_connections.contains(socket)) {
              timer->stop();
              return;
            }
            const qint64 remaining = body.size() - *offset;
            if (remaining <= 0) {
              timer->stop();
              m_lastSlowDripBytesWritten[path] = *offset;
              socket->disconnectFromHost();
              return;
            }
            const qint64 sliceSize = std::min<qint64>(chunkSize, remaining);
            const qint64 written =
                socket->write(body.constData() + *offset, sliceSize);
            if (written <= 0) {
              timer->stop();
              m_lastSlowDripBytesWritten[path] = *offset;
              return;
            }
            *offset += written;
            m_lastSlowDripBytesWritten[path] = *offset;
          });
  connect(socket, &QTcpSocket::disconnected, this,
          [timer]() { timer->stop(); });
  timer->start();
}
