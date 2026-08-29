#include "MockHttpServer.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <utility>

MockHttpServer::MockHttpServer(QObject *parent)
    : QObject(parent), m_server(new QTcpServer(this)) {
  connect(m_server, &QTcpServer::newConnection, this,
          &MockHttpServer::onNewConnection);
  const bool listening = m_server->listen(QHostAddress::LocalHost, 0);
  // Q_ASSERT is compiled out in release builds (or with QT_NO_DEBUG
  // defined), which would let a listen() failure pass silently here --
  // every test using this class would then fail downstream in a
  // confusing way (e.g. port() == 0, connection-refused errors) rather
  // than at the actual point of failure. qFatal() is enforced in all
  // build types, so a bind failure is always reported immediately and
  // unambiguously, with the real errorString() attached.
  if (!listening) {
    qFatal("MockHttpServer failed to listen on 127.0.0.1: %s",
           qPrintable(m_server->errorString()));
  }
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
  return m_allHeaderNamesEverSeen.contains(lowerHeaderName);
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
  for (auto headerIt = connection.headers.keyBegin();
       headerIt != connection.headers.keyEnd(); ++headerIt) {
    m_allHeaderNamesEverSeen.insert(*headerIt);
  }
  emit requestHandled(path);

  const Response response = m_responses.value(path, Response{});
  writeResponse(connection.socket, path, response, connection.headers);
}

void MockHttpServer::writeResponse(
    QTcpSocket *socket, const QString &path, const Response &response,
    const QHash<QByteArray, QByteArray> &requestHeaders) {
  // Conditional-match check: an exact If-None-Match match against the
  // configured ETag, or an exact If-Modified-Since match against the
  // configured lastModifiedForConditionalMatch, answers with a bodyless
  // 304 instead of the full response.
  const bool etagMatches =
      !response.etagForConditionalMatch.isEmpty() &&
      requestHeaders.value("if-none-match") == response.etagForConditionalMatch;
  const bool lastModifiedMatches =
      !response.lastModifiedForConditionalMatch.isEmpty() &&
      requestHeaders.value("if-modified-since") ==
          response.lastModifiedForConditionalMatch;

  if (etagMatches || lastModifiedMatches) {
    QByteArray out = "HTTP/1.1 304 Not Modified\r\n";
    const QByteArray etagToSend = !response.etagOn304Override.isEmpty()
                                      ? response.etagOn304Override
                                      : response.etagForConditionalMatch;
    const QByteArray lastModifiedToSend =
        !response.lastModifiedOn304Override.isEmpty()
            ? response.lastModifiedOn304Override
            : response.lastModifiedForConditionalMatch;
    if (!etagToSend.isEmpty()) {
      out += "ETag: " + etagToSend + "\r\n";
    }
    if (!lastModifiedToSend.isEmpty()) {
      out += "Last-Modified: " + lastModifiedToSend + "\r\n";
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
  // The timer is a plain QObject child of `this` (NOT a shared_ptr
  // captured inside its own timeout lambda): capturing a shared_ptr<QTimer>
  // in a lambda connected to that same timer's own timeout signal creates
  // a reference cycle (timer -> connection -> lambda -> shared_ptr ->
  // timer) that keeps the timer alive forever, even after it stops
  // firing. Parenting to `this` guarantees cleanup even if some early
  // return path here were ever missed.
  auto *timer = new QTimer(this);
  timer->setInterval(chunkDelayMs);

  // `body` is moved (not copied) into the lambda: `writeSlowDrip()`
  // already took it by value (one copy from the caller), so capturing
  // it by value again here would make a second full copy of the
  // response body that lives for the whole slow-drip duration -- wasted
  // memory that scales with fixture size (e.g. multi-megabyte
  // incremental-cap test bodies). Since `body` is never read again in
  // this function after this point, moving it is free and behaviourally
  // identical.
  connect(timer, &QTimer::timeout, this,
          [this, socket, path, body = std::move(body), chunkSize, timer,
           offset = qint64{0}]() mutable {
            if (!m_connections.contains(socket)) {
              timer->stop();
              timer->deleteLater();
              return;
            }
            const qint64 remaining = body.size() - offset;
            if (remaining <= 0) {
              timer->stop();
              timer->deleteLater();
              m_lastSlowDripBytesWritten[path] = offset;
              socket->disconnectFromHost();
              return;
            }
            const qint64 sliceSize = std::min<qint64>(chunkSize, remaining);
            const qint64 written =
                socket->write(body.constData() + offset, sliceSize);
            if (written <= 0) {
              timer->stop();
              timer->deleteLater();
              m_lastSlowDripBytesWritten[path] = offset;
              return;
            }
            offset += written;
            m_lastSlowDripBytesWritten[path] = offset;
          });
  // Copilot review (round 31, high severity): the timeout lambda above
  // can already have called timer->deleteLater() on several early-return
  // paths (e.g. written <= 0) before the socket's disconnected signal
  // fires. Using `timer` itself as the receiver/context object (rather
  // than `this`) means Qt automatically severs this connection once
  // `timer`'s destructor actually runs, so this lambda can never be
  // invoked with a dangling `timer` pointer -- unlike a `this`-scoped
  // connection, which stays alive for MockHttpServer's whole lifetime
  // and would happily invoke the capture-by-raw-pointer lambda even
  // after the timer it points to had already been destroyed.
  connect(socket, &QTcpSocket::disconnected, timer, [timer]() {
    timer->stop();
    timer->deleteLater();
  });
  timer->start();
}
