#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QUrl>

class QTcpServer;
class QTcpSocket;
class QTimer;

// A minimal, deterministic loopback HTTP/1.1 mock server used by the
// Asset* network/coordinator tests to exercise AssetNetworkFetcher and
// AssetRequestCoordinator against real socket-level behaviour (byte-level
// incremental delivery, actual request headers as sent by
// QNetworkAccessManager, real connection resets) rather than a stubbed
// QNetworkReply -- per djensenius/ArkhamHorror-Linux#17's requirement for
// "a deterministic local mock server" whose tests "assert actual dispatch
// / overlap rather than vacuous queued callbacks."
//
// One response per registered path is served for every request to that
// path (tests reconfigure between assertions as needed); every response
// closes the connection afterwards (no keep-alive), which is simplest and
// sufficient for these tests and never itself masks a correctness bug in
// AssetNetworkFetcher (which must tolerate a fresh connection per request
// regardless).
class MockHttpServer final : public QObject {
  Q_OBJECT
public:
  struct Response {
    int status = 200;
    QByteArray reasonPhrase = "OK";
    QByteArray contentType;
    QByteArray body;
    // Extra raw header lines, e.g. {"Location", "http://..."} or
    // {"ETag", "\"abc\""}. Content-Type/Content-Length are added
    // automatically and must not be duplicated here.
    QList<QPair<QByteArray, QByteArray>> extraHeaders;

    // If set, a request whose If-None-Match exactly matches
    // etagForConditionalMatch, OR whose If-Modified-Since exactly matches
    // lastModifiedForConditionalMatch, is answered with a bodyless 304
    // (echoing whichever configured validator(s) are non-empty) instead
    // of this full response.
    QByteArray etagForConditionalMatch;
    QByteArray lastModifiedForConditionalMatch;
    // When non-empty, overrides the ETag/Last-Modified header VALUE sent
    // on that bodyless 304 response, independent of what was matched
    // against (etagForConditionalMatch/lastModifiedForConditionalMatch
    // above). Models a server rotating/extending its validator at
    // revalidation time (RFC 7232 S4.1 permits a 304 to carry a
    // different validator than the one the client asked about). Only
    // takes effect once a 304 is already being sent; a non-matching
    // request is unaffected.
    QByteArray etagOn304Override;
    QByteArray lastModifiedOn304Override;

    // When true, `body` is written to the socket in small delayed slices
    // (see chunkSize/chunkDelayMs) instead of all at once, so a test can
    // observe a client aborting mid-stream (proving enforcement happens
    // incrementally, not only once the full body has already arrived).
    bool slowDrip = false;
    int chunkSize = 4096;
    int chunkDelayMs = 5;

    // When true, the connection is closed immediately after writing the
    // status line + headers, before any body byte -- simulates a
    // mid-response transport failure.
    bool resetAfterHeaders = false;
  };

  explicit MockHttpServer(QObject *parent = nullptr);
  ~MockHttpServer() override;

  [[nodiscard]] quint16 port() const;
  [[nodiscard]] QUrl baseUrlFor(const QString &path) const;

  void setResponse(const QString &path, Response response);

  [[nodiscard]] int requestCount(const QString &path) const;
  // Lower-cased header name -> value, for the LAST request received at
  // `path` (sufficient for the "no cookie/auth header ever sent" and
  // "conditional headers arrived correctly" assertions these tests need).
  [[nodiscard]] QHash<QByteArray, QByteArray> lastRequestHeaders(const QString &path) const;
  // True iff ANY request to ANY path, ever, carried this (lower-cased)
  // header name -- tracked via a running set of every header name seen
  // across every request this server has ever handled (NOT merely the
  // last request per path), so an assertion that a header is never sent
  // cannot be defeated by a later request to the same path omitting it.
  [[nodiscard]] bool anyRequestEverHadHeader(const QByteArray &lowerHeaderName) const;

  // Number of bytes of a slowDrip response body actually flushed to the
  // socket for the most recent request at `path`, even if the client
  // disconnected before the full body was sent. Used to prove an
  // incremental byte-cap abort happened strictly before the full
  // (oversized) body was transmitted.
  [[nodiscard]] qint64 lastBytesWrittenForSlowDrip(const QString &path) const;

signals:
  void requestHandled(QString path);

private slots:
  void onNewConnection();

private:
  struct Connection {
    QTcpSocket *socket{nullptr};
    QByteArray buffer;
    bool headersComplete{false};
    QString path;
    QHash<QByteArray, QByteArray> headers;
  };

  void onReadyRead(QTcpSocket *socket);
  void onDisconnected(QTcpSocket *socket);
  void tryParseAndRespond(Connection &connection);
  void writeResponse(QTcpSocket *socket, const QString &path, const Response &response,
                     const QHash<QByteArray, QByteArray> &requestHeaders);
  void writeSlowDrip(QTcpSocket *socket, const QString &path, QByteArray body,
                     int chunkSize, int chunkDelayMs);

  QTcpServer *m_server;
  QHash<QString, Response> m_responses;
  QHash<QString, int> m_requestCounts;
  QHash<QString, QHash<QByteArray, QByteArray>> m_lastRequestHeaders;
  QSet<QByteArray> m_allHeaderNamesEverSeen;
  QHash<QString, qint64> m_lastSlowDripBytesWritten;
  QHash<QTcpSocket *, Connection> m_connections;
};
