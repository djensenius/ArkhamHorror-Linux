#include "AssetNetworkFetcher.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QImageReader>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QtAssert>
#include <cstring>
#include <stdexcept>
#include <utility>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

// Returns the AssetFormat whose magic bytes match `bytes`, or std::nullopt
// if none of the three supported formats' signatures match. This is an
// independent check from the declared Content-Type: both must agree with
// the caller's expected format for a fetch to succeed.
std::optional<AssetFormat> sniffMagicBytes(const QByteArray &bytes) {
  static const QByteArray kPngSignature =
      QByteArrayLiteral("\x89PNG\r\n\x1a\n");
  if (bytes.startsWith(kPngSignature)) {
    return AssetFormat::Png;
  }
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
      static_cast<unsigned char>(bytes[1]) == 0xD8 &&
      static_cast<unsigned char>(bytes[2]) == 0xFF) {
    return AssetFormat::Jpeg;
  }
  // AVIF is ISOBMFF: a leading 4-byte big-endian box size, then "ftyp",
  // then a 4-byte major brand, then zero or more 4-byte compatible brands
  // filling out the rest of the box (whose total size is the leading
  // 4-byte field). Accept if the major brand OR any compatible brand is
  // "avif" (still image) or "avis" (image sequence).
  if (bytes.size() >= 12 && bytes.mid(4, 4) == QByteArrayLiteral("ftyp")) {
    const quint32 boxSize = (static_cast<unsigned char>(bytes[0]) << 24) |
                            (static_cast<unsigned char>(bytes[1]) << 16) |
                            (static_cast<unsigned char>(bytes[2]) << 8) |
                            static_cast<unsigned char>(bytes[3]);
    const qint64 available = bytes.size();
    // Per ISO/IEC 14496-12, a box size of 0 means "this box extends to
    // the end of the enclosing file/buffer" -- NOT a zero-length box.
    // Treating it as zero-length would silently reject a spec-valid
    // AVIF `ftyp` box as a magic-bytes mismatch.
    const qint64 boxEnd =
        boxSize == 0 ? available : qMin<qint64>(boxSize, available);
    // A ftyp box must declare at least 12 bytes (4-byte size + "ftyp" +
    // 4-byte major_brand) to even contain a major_brand field. A nonzero
    // declared size smaller than that is a malformed/truncated box: it
    // must never be treated as ftyp data, because reading major_brand at
    // a fixed offset of 8 regardless of boxEnd would read past the
    // declared (invalid) box boundary into whatever bytes happen to
    // follow -- potentially misclassifying non-AVIF data as AVIF.
    if (boxEnd >= 12) {
      // Layout: [0..4) box size, [4..8) "ftyp", [8..12) major_brand,
      // [12..16) minor_version (a version number, NOT a brand -- it must
      // never be compared against "avif"/"avis"), [16..boxEnd) zero or
      // more 4-byte compatible_brands. Checking major_brand and then
      // skipping straight to compatible_brands (offset 16) avoids
      // misclassifying a crafted minor_version as a brand match.
      //
      // Compare directly against bytes.constData() rather than slicing
      // with mid(): a crafted box can declare boxEnd up to the full
      // capped download size (maxEncodedBytes, 20 MiB), which would
      // otherwise drive up to ~5 million loop iterations, each
      // allocating (and immediately discarding) a 4-byte QByteArray --
      // an avoidable CPU/allocation cost inflicted by network-controlled
      // bytes.
      const char *const data = bytes.constData();
      const auto isBrandAt = [data](qint64 offset, const char *brand) {
        return std::memcmp(data + offset, brand, 4) == 0;
      };
      if (isBrandAt(8, "avif") || isBrandAt(8, "avis")) {
        return AssetFormat::Avif;
      }
      for (qint64 offset = 16; offset + 4 <= boxEnd; offset += 4) {
        if (isBrandAt(offset, "avif") || isBrandAt(offset, "avis")) {
          return AssetFormat::Avif;
        }
      }
    }
  }
  return std::nullopt;
}

// Review item 10: Qt's bundled libjpeg-based decoder tolerates a
// premature end-of-file within entropy-coded scan data by *synthesising*
// a missing End-Of-Image (EOI, the two-byte marker 0xFF 0xD9) marker: it
// logs a "premature end of data segment" warning but still returns a
// non-null, seemingly-valid QImage. A response body truncated by a
// flaky/adversarial network mid-transfer can therefore decode
// "successfully" through QImageReader despite genuinely missing its
// tail. This performs an independent, bounded, single-pass scan of the
// JPEG marker structure to determine whether a *genuine* EOI marker is
// actually present in the supplied bytes, distinct from (and run before
// trusting) Qt's own decode.
//
// Trailing-data policy: once a genuine EOI marker is found, the scan
// stops and reports success immediately -- any bytes that might follow
// EOI (e.g. padding, or another concatenated stream) are deliberately
// not inspected. Only bytes that never reach a genuine EOI at all (a
// truncated response) are rejected here.
//
// This does not replace normal magic-byte or QImageReader
// validation -- a body that fails this check is prevented from ever
// reaching the decoder at all, and a body that passes still goes
// through the existing dimension/limit/decode checks below.
bool jpegCodestreamHasGenuineEoi(const QByteArray &bytes) {
  const qint64 size = bytes.size();
  const auto *const data =
      reinterpret_cast<const unsigned char *>(bytes.constData());
  // Magic-byte sniffing has already confirmed bytes[0..2] == FF D8 FF,
  // so start scanning markers right after the two-byte SOI.
  if (size < 2) {
    return false;
  }
  qint64 pos = 2;
  while (pos < size) {
    if (data[pos] != 0xFF) {
      // A marker was expected here but the byte isn't 0xFF: the
      // structure itself is malformed. Leave that diagnosis to the
      // normal QImageReader decode path (MalformedImage) rather than
      // misreporting it as a codestream-completeness failure.
      return false;
    }
    // Marker codes may be preceded by any number of 0xFF fill bytes.
    while (pos < size && data[pos] == 0xFF) {
      ++pos;
    }
    if (pos >= size) {
      return false; // ran out of bytes while skipping fill bytes
    }
    const unsigned char marker = data[pos];
    ++pos;
    if (marker == 0xD9) {
      return true; // genuine EOI actually present in the supplied bytes
    }
    if (marker == 0x00) {
      return false; // a stuffed byte can only appear inside scan data
    }
    if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
      continue; // RSTn / TEM: standalone, no length field or payload
    }
    if (marker == 0xD8) {
      return false; // unexpected second top-level SOI: malformed
    }
    // Every other marker (SOF*, DHT, DQT, DRI, APPn, COM, SOS, ...) is
    // followed by a 2-byte big-endian length, INCLUDING those 2 bytes.
    if (pos + 1 >= size) {
      return false;
    }
    const int segmentLength =
        (static_cast<int>(data[pos]) << 8) | static_cast<int>(data[pos + 1]);
    if (segmentLength < 2) {
      return false; // a length that can't even cover its own field
    }
    if (marker != 0xDA) {
      // Non-scan segment: skip its declared payload wholesale.
      pos += segmentLength;
      if (pos > size) {
        return false; // declared length runs past the available bytes
      }
      continue;
    }
    // SOS (Start Of Scan): skip its own header, then scan the
    // entropy-coded data that follows byte-by-byte. Within scan data, a
    // 0xFF byte only terminates the scan if the following byte is a
    // "real" marker code -- 0xFF 0x00 is a stuffed literal 0xFF data
    // byte, 0xFF 0xD0-0xD7 is a restart marker (both stay inside the
    // scan), and a run of 0xFF fill bytes just keeps scanning.
    pos += segmentLength;
    if (pos > size) {
      return false;
    }
    while (pos < size) {
      if (data[pos] != 0xFF) {
        ++pos;
        continue;
      }
      if (pos + 1 >= size) {
        return false; // truncated exactly on a trailing 0xFF byte
      }
      const unsigned char next = data[pos + 1];
      if (next == 0x00 || (next >= 0xD0 && next <= 0xD7)) {
        pos += 2; // stuffed byte or restart marker: still scan data
        continue;
      }
      if (next == 0xFF) {
        ++pos; // fill byte: keep looking at the byte after it
        continue;
      }
      break; // a genuine marker follows: hand control back to the
             // outer loop, which will interpret it (possibly another
             // SOS for a progressive JPEG, or the final EOI).
    }
  }
  return false; // exhausted the buffer without ever finding a genuine EOI
}

// Normalises a Content-Type header value to a bare, lowercase media type
// (e.g. "image/png; charset=binary" -> "image/png"), so a parameter suffix
// cannot defeat the comparison in either direction.
QString normalizeContentType(const QString &raw) {
  const qsizetype semi = raw.indexOf(u';');
  const QString mediaType = (semi >= 0) ? raw.left(semi) : raw;
  return mediaType.trimmed().toLower();
}

// Whether Qt's installed image plugins can decode `format`, and (if so)
// which exact plugin-key spelling QImageReader should be constructed
// with. These two questions are answered together, from the SAME
// QImageReader::supportedImageFormats() snapshot, specifically so the
// format-hint given to QImageReader can never diverge from the spelling
// that was actually confirmed supported: a fixed hint independent of this
// check (e.g. always "jpeg") could pass the support check on a Qt build
// that only registers the OTHER JPEG key ("jpg") yet still fail to
// decode, since QImageReader's format hint is matched by exact plugin key.
// JPEG is checked/hinted under both of Qt's registered plugin keys
// ("jpeg" and "jpg" -- both are advertised by the stock qjpeg plugin, but
// a build could plausibly register only one). AVIF/PNG have exactly one
// Qt key each.
struct QtCodecSupport {
  bool supported{false};
  QByteArray formatHint;
};

QtCodecSupport resolveQtCodecSupport(AssetFormat format) {
  const QList<QByteArray> supported = QImageReader::supportedImageFormats();
  switch (format) {
  case AssetFormat::Avif:
    return {supported.contains(QByteArrayLiteral("avif")),
            QByteArrayLiteral("avif")};
  case AssetFormat::Jpeg:
    if (supported.contains(QByteArrayLiteral("jpeg"))) {
      return {true, QByteArrayLiteral("jpeg")};
    }
    if (supported.contains(QByteArrayLiteral("jpg"))) {
      return {true, QByteArrayLiteral("jpg")};
    }
    // Neither key is registered: report unsupported. The hint value here
    // is never actually used to construct a QImageReader in that case
    // (the caller checks `supported` first), but is filled in for
    // completeness.
    return {false, QByteArrayLiteral("jpeg")};
  case AssetFormat::Png:
    return {supported.contains(QByteArrayLiteral("png")),
            QByteArrayLiteral("png")};
  }
  Q_UNREACHABLE_RETURN((QtCodecSupport{}));
}

void applyCommonRequestSettings(QNetworkRequest &request) {
  request.setRawHeader("Accept", "image/avif,image/jpeg,image/png");
  request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                       QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                       QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,
                       QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                       QNetworkRequest::AlwaysNetwork);
  request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
}

} // namespace

AssetNetworkFetcher::AssetNetworkFetcher(Limits limits,
                                         std::chrono::milliseconds timeout,
                                         QObject *parent)
    : AssetNetworkFetcher(std::make_unique<QNetworkAccessManager>(), nullptr,
                          limits, timeout, parent) {}

AssetNetworkFetcher::AssetNetworkFetcher(QNetworkAccessManager &nam,
                                         Limits limits,
                                         std::chrono::milliseconds timeout,
                                         QObject *parent)
    : AssetNetworkFetcher(nullptr, &nam, limits, timeout, parent) {}

AssetNetworkFetcher::AssetNetworkFetcher(
    std::unique_ptr<QNetworkAccessManager> ownedNam,
    QNetworkAccessManager *borrowedNam, Limits limits,
    std::chrono::milliseconds timeout, QObject *parent)
    : QObject(parent), m_ownedNam(std::move(ownedNam)),
      m_nam(m_ownedNam ? *m_ownedNam : *borrowedNam), m_limits(limits),
      m_timeout(timeout) {
  if (timeout < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("asset fetcher timeout cannot be negative");
  }
  if (limits.maxEncodedBytes <= 0 || limits.maxDimensionPixels <= 0 ||
      limits.maxTotalPixels <= 0) {
    throw std::invalid_argument("asset fetcher limits must be positive");
  }
}

AssetNetworkFetcher::~AssetNetworkFetcher() {
  for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
    if (QTimer *timer = it.value().timer) {
      timer->stop();
    }
    QNetworkReply *reply = it.value().reply;
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }
  m_pending.clear();
}

AssetNetworkFetcher::FetchHandle
AssetNetworkFetcher::fetch(const QUrl &url, AssetFormat expectedFormat,
                           ConditionalHeaders conditional,
                           FetchCallback callback) {
  // Fail closed on any scheme other than http/https. This class is
  // documented (and, via AssetLocator's UrlValidator::validateCustomUrl()
  // gate, currently only ever invoked) as an HTTP(S)-only fetcher -- but
  // QNetworkAccessManager itself happily services other schemes it
  // supports (e.g. file://, qrc://). Without this explicit, independent
  // check here, a future caller that ever passed this class an
  // unvalidated URL (bypassing AssetLocator) could read arbitrary local
  // files rather than fetching over the network. Checked and dispatched
  // BEFORE any QNetworkRequest/QNetworkReply is created, so a rejected
  // scheme never reaches QNetworkAccessManager at all.
  const QString scheme = url.scheme();
  if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
    QPointer<AssetNetworkFetcher> self(this);
    QMetaObject::invokeMethod(
        this,
        [self, callback = std::move(callback)]() mutable {
          if (self) {
            std::move(callback)(AssetOutcome<ConditionalFetchResult>(AssetError{
                AssetErrorCode::UnsupportedScheme,
                QStringLiteral("only http and https URLs may be fetched")}));
          }
        },
        Qt::QueuedConnection);
    // Copilot review (round 29, medium severity): this rejection is
    // dispatched asynchronously (to preserve fetch()'s always-async
    // callback contract) but nothing is ever inserted into m_pending for
    // it, so there is genuinely no in-flight operation cancel() could
    // intercept -- calling cancel() on a handle for this path could
    // therefore never actually cancel the still-queued UnsupportedScheme
    // delivery, which would contradict cancel()'s documented contract
    // that a successful cancellation always yields
    // AssetErrorCode::Cancelled instead of the operation's real outcome.
    // Returning an invalid handle (rather than consuming a real
    // m_nextHandle value) makes that contract unambiguous: callers can
    // tell from isValid() alone that this handle was never eligible for
    // cancel() to begin with, instead of having to learn via a stale/
    // unknown-handle no-op that happens to look identical to cancelling
    // an already-completed request.
    return FetchHandle{};
  }

  QNetworkRequest request(url);
  applyCommonRequestSettings(request);
  if (!conditional.etag.isEmpty()) {
    request.setRawHeader("If-None-Match", conditional.etag.toUtf8());
  }
  if (!conditional.lastModified.isEmpty()) {
    request.setRawHeader("If-Modified-Since",
                         conditional.lastModified.toUtf8());
  }

  QNetworkReply *reply = m_nam.get(request);
  // Bound QNetworkReply's own internal socket-level read buffer to the
  // same cap this class enforces on Pending::buffer (plus one byte, to
  // match the "+1" over-read this class's readyRead handler already uses
  // to detect an overflow without ever letting its own buffer exceed the
  // cap -- see handleReadyRead()). Without this, a large/fast response
  // could still let Qt buffer well past maxEncodedBytes internally, in
  // its own reply object, in between this class's readyRead-driven reads
  // -- this makes the "never exceeds maxEncodedBytes" bound apply to the
  // whole pipeline, not just the copy this class keeps in Pending::buffer.
  reply->setReadBufferSize(m_limits.maxEncodedBytes + 1);
  const quint64 handle = m_nextHandle++;

  Pending pending;
  pending.reply = reply;
  pending.expectedFormat = expectedFormat;
  pending.conditionalRequested = !conditional.isEmpty();
  pending.callback = std::move(callback);

  QTimer *timer = nullptr;
  if (m_timeout.count() > 0) {
    timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, handle]() {
      auto it = m_pending.find(handle);
      if (it == m_pending.end()) {
        return;
      }
      QNetworkReply *r = it.value().reply;
      // The timer that just fired is this very request's own timer: it
      // is single-shot (never fires again) but is NOT otherwise deleted
      // anywhere else on this path, so it must be cleaned up here --
      // otherwise every timed-out request leaks one QTimer for the
      // remaining lifetime of this AssetNetworkFetcher.
      QTimer *t = it.value().timer;
      QObject::disconnect(r, nullptr, this, nullptr);
      r->abort();
      r->deleteLater();
      if (t) {
        t->deleteLater();
      }
      completeWithError(handle,
                        AssetError{AssetErrorCode::Transport,
                                   QStringLiteral("request timed out")});
    });
  }
  pending.timer = timer;

  m_pending.insert(handle, std::move(pending));

  connect(reply, &QNetworkReply::readyRead, this,
          [this, handle]() { handleReadyRead(handle); });
  connect(reply, &QNetworkReply::finished, this,
          [this, handle]() { handleFinished(handle); });

  if (timer) {
    timer->start(m_timeout);
  }

  return FetchHandle{handle};
}

void AssetNetworkFetcher::handleReadyRead(quint64 handle) {
  auto it = m_pending.find(handle);
  if (it == m_pending.end()) {
    return;
  }
  Pending &pending = it.value();
  if (pending.overflowed) {
    return; // already aborting; ignore further readyRead delivery
  }
  // Never read (and therefore never buffer) more than the remaining
  // budget, plus exactly one extra byte solely to detect that the server
  // tried to send more than the cap allows: appending readAll() first and
  // checking the size afterwards would let pending.buffer briefly grow
  // past m_limits.maxEncodedBytes before this function aborts, weakening
  // the hard bound the class comment promises.
  const qint64 remaining = m_limits.maxEncodedBytes - pending.buffer.size();
  const QByteArray chunk = pending.reply->read(remaining + 1);
  if (chunk.size() > remaining) {
    // Abort immediately: never buffer past the configured cap, regardless
    // of how large a hostile or misconfigured server claims (or omits)
    // Content-Length to be.
    pending.buffer.append(chunk.left(remaining));
    pending.overflowed = true;
    // Stop the per-request timeout timer here, not just in
    // handleFinished() once the abort's finished() signal is actually
    // delivered: abort() does not guarantee finished() fires
    // synchronously, so without this a timeout whose interval elapses in
    // that gap would fire first and misreport this exact
    // ResponseTooLarge outcome as a generic Transport (timeout) error
    // instead.
    if (pending.timer) {
      pending.timer->stop();
    }
    pending.reply->abort();
    return;
  }
  pending.buffer.append(chunk);
}

void AssetNetworkFetcher::completeWithError(quint64 handle, AssetError error) {
  auto it = m_pending.find(handle);
  if (it == m_pending.end()) {
    return;
  }
  FetchCallback callback = std::move(it.value().callback);
  m_pending.erase(it);

  QPointer<AssetNetworkFetcher> self(this);
  QMetaObject::invokeMethod(
      this,
      [self, callback = std::move(callback),
       error = std::move(error)]() mutable {
        if (self) {
          std::move(callback)(AssetOutcome<ConditionalFetchResult>(error));
        }
      },
      Qt::QueuedConnection);
}

void AssetNetworkFetcher::handleFinished(quint64 handle) {
  auto it = m_pending.find(handle);
  if (it == m_pending.end()) {
    return; // already handled (cancel/timeout/overflow completion path)
  }
  Pending pendingCopy = std::move(it.value());
  m_pending.erase(it);

  if (QTimer *t = pendingCopy.timer) {
    t->stop();
    t->deleteLater();
  }
  QNetworkReply *reply = pendingCopy.reply;
  reply->deleteLater();

  auto emitResult = [this, callback = std::move(pendingCopy.callback)](
                        AssetOutcome<ConditionalFetchResult> result) mutable {
    QPointer<AssetNetworkFetcher> self(this);
    QMetaObject::invokeMethod(
        this,
        [self, callback = std::move(callback),
         result = std::move(result)]() mutable {
          if (self) {
            std::move(callback)(std::move(result));
          }
        },
        Qt::QueuedConnection);
  };

  if (pendingCopy.overflowed) {
    emitResult(AssetError{AssetErrorCode::ResponseTooLarge,
                          QStringLiteral("response body exceeded the "
                                         "configured encoded-size limit")});
    return;
  }

  const QVariant statusAttr =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
  if (!statusAttr.isValid()) {
    if (reply->error() == QNetworkReply::OperationCanceledError) {
      emitResult(AssetError{AssetErrorCode::Cancelled,
                            QStringLiteral("request was cancelled")});
    } else {
      emitResult(AssetError{AssetErrorCode::Transport,
                            QStringLiteral("network transport error: %1")
                                .arg(reply->errorString())});
    }
    return;
  }

  const int status = statusAttr.toInt();

  if (status == 304) {
    if (!pendingCopy.conditionalRequested) {
      emitResult(AssetError{
          AssetErrorCode::ConditionalWithoutCachedBody,
          QStringLiteral("server returned 304 for an unconditional "
                         "request; no cached body is available to reuse")});
      return;
    }
    ConditionalFetchResult result;
    result.notModified = true;
    // A 304 MAY carry refreshed validators (RFC 7232 S4.1) even with no
    // body; capture them so the coordinator can extend the cache entry's
    // validator instead of forever revalidating against a value the
    // origin may eventually stop recognising.
    result.refreshedEtag = QString::fromLatin1(reply->rawHeader("ETag"));
    result.refreshedLastModified =
        QString::fromLatin1(reply->rawHeader("Last-Modified"));
    emitResult(std::move(result));
    return;
  }

  if (status >= 300 && status < 400) {
    // Never auto-followed (ManualRedirectPolicy); every 3xx is explicit
    // failure so a redirect can never smuggle a request to another origin.
    emitResult(AssetError{AssetErrorCode::RedirectRejected,
                          QStringLiteral("server responded with a redirect; "
                                         "redirects are never followed")});
    return;
  }

  if (status == 404) {
    emitResult(AssetError{AssetErrorCode::NotFound,
                          QStringLiteral("asset was not found (404)")});
    return;
  }

  if (status < 200 || status >= 300) {
    emitResult(AssetError{
        AssetErrorCode::UnexpectedStatus,
        QStringLiteral("server responded with unexpected HTTP status %1")
            .arg(status)});
    return;
  }

  if (reply->error() != QNetworkReply::NoError) {
    emitResult(AssetError{AssetErrorCode::Transport,
                          QStringLiteral("network transport error: %1")
                              .arg(reply->errorString())});
    return;
  }

  // Drain any final buffered bytes not yet delivered via readyRead, using
  // the same bounded-read-plus-one-overflow-byte technique as
  // handleReadyRead(): calling readAll() unconditionally and checking the
  // size afterward would let pendingCopy.buffer briefly grow past
  // m_limits.maxEncodedBytes before this check runs, contradicting the
  // "buffer never exceeds the cap" hard bound the class comment promises.
  const qint64 remainingBudget =
      m_limits.maxEncodedBytes - pendingCopy.buffer.size();
  const QByteArray tail = reply->read(remainingBudget + 1);
  if (tail.size() > remainingBudget) {
    emitResult(AssetError{AssetErrorCode::ResponseTooLarge,
                          QStringLiteral("response body exceeded the "
                                         "configured encoded-size limit")});
    return;
  }
  pendingCopy.buffer.append(tail);

  const QByteArray &body = pendingCopy.buffer;

  const QString declaredContentType = normalizeContentType(
      QString::fromLatin1(reply->rawHeader("Content-Type")));
  const QString expectedMime = assetFormatMimeType(pendingCopy.expectedFormat);
  if (declaredContentType != expectedMime) {
    emitResult(AssetError{
        AssetErrorCode::ContentTypeMismatch,
        QStringLiteral("declared Content-Type \"%1\" does not match the "
                       "expected asset format")
            .arg(declaredContentType)});
    return;
  }

  AssetOutcome<QImage> decodedOutcome =
      decodeAndValidate(body, pendingCopy.expectedFormat);
  if (!decodedOutcome) {
    emitResult(decodedOutcome.error());
    return;
  }

  FetchedAsset asset;
  asset.encodedBytes = body;
  asset.contentType = declaredContentType;
  asset.dimensions = decodedOutcome->size();
  asset.decodedImage = std::move(*decodedOutcome);
  asset.sha256Hex = QString::fromLatin1(
      QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex());
  asset.etag = QString::fromLatin1(reply->rawHeader("ETag"));
  asset.lastModified = QString::fromLatin1(reply->rawHeader("Last-Modified"));
  asset.httpStatus = status;

  ConditionalFetchResult result;
  result.notModified = false;
  result.asset = std::move(asset);
  emitResult(std::move(result));
}

AssetOutcome<QImage>
AssetNetworkFetcher::decodeAndValidate(const QByteArray &encodedBytes,
                                       AssetFormat expectedFormat) const {
  const std::optional<AssetFormat> sniffed = sniffMagicBytes(encodedBytes);
  if (!sniffed || *sniffed != expectedFormat) {
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MagicBytesMismatch,
        QStringLiteral("body's magic bytes do not match the expected "
                       "asset format")});
  }

  if (expectedFormat == AssetFormat::Jpeg &&
      !jpegCodestreamHasGenuineEoi(encodedBytes)) {
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("JPEG body has no genuine End-Of-Image marker "
                       "(response was likely truncated in transit)")});
  }

  const QtCodecSupport codecSupport = resolveQtCodecSupport(expectedFormat);
  if (!codecSupport.supported) {
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::UnsupportedCodec,
        QStringLiteral("no installed Qt image plugin can decode this "
                       "asset's format")});
  }

  QBuffer buffer;
  buffer.setData(encodedBytes);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer, codecSupport.formatHint);

  const QSize declaredSize = reader.size();
  if (!declaredSize.isValid() || declaredSize.width() <= 0 ||
      declaredSize.height() <= 0) {
    return AssetOutcome<QImage>(
        AssetError{AssetErrorCode::MalformedImage,
                   QStringLiteral("image header is malformed or reports "
                                  "non-positive dimensions")});
  }
  if (declaredSize.width() > m_limits.maxDimensionPixels ||
      declaredSize.height() > m_limits.maxDimensionPixels) {
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::DimensionTooLarge,
        QStringLiteral("image dimension %1x%2 exceeds the configured cap "
                       "of %3 pixels per side")
            .arg(declaredSize.width())
            .arg(declaredSize.height())
            .arg(m_limits.maxDimensionPixels)});
  }
  // 64-bit multiplication: both operands are already bounded above by
  // maxDimensionPixels (a configurable but always-small int), so this can
  // never overflow even at the largest permitted configuration.
  const qint64 totalPixels =
      static_cast<qint64>(declaredSize.width()) * declaredSize.height();
  if (totalPixels > m_limits.maxTotalPixels) {
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::PixelBudgetExceeded,
        QStringLiteral("image totals %1 pixels, exceeding the configured "
                       "cap of %2")
            .arg(totalPixels)
            .arg(m_limits.maxTotalPixels)});
  }

  QImage decoded = reader.read();
  if (decoded.isNull()) {
    if (reader.error() == QImageReader::UnsupportedFormatError) {
      return AssetOutcome<QImage>(AssetError{
          AssetErrorCode::UnsupportedCodec,
          QStringLiteral("installed Qt image plugin declined to decode "
                         "this asset")});
    }
    return AssetOutcome<QImage>(
        AssetError{AssetErrorCode::MalformedImage,
                   QStringLiteral("image body failed to decode: %1")
                       .arg(reader.errorString())});
  }

  return AssetOutcome<QImage>(std::move(decoded));
}

void AssetNetworkFetcher::cancel(FetchHandle handle) {
  if (!handle.isValid()) {
    return;
  }
  auto it = m_pending.find(handle.id);
  if (it == m_pending.end()) {
    return; // stale or already-completed handle: safe no-op
  }
  if (QTimer *timer = it.value().timer) {
    timer->stop();
    timer->deleteLater();
  }
  QNetworkReply *reply = it.value().reply;
  FetchCallback callback = std::move(it.value().callback);
  QObject::disconnect(reply, nullptr, this, nullptr);
  m_pending.erase(it);
  reply->abort();
  reply->deleteLater();

  QPointer<AssetNetworkFetcher> self(this);
  QMetaObject::invokeMethod(
      this,
      [self, callback = std::move(callback)]() mutable {
        if (self) {
          std::move(callback)(AssetOutcome<ConditionalFetchResult>(
              AssetError{AssetErrorCode::Cancelled,
                         QStringLiteral("request was cancelled")}));
        }
      },
      Qt::QueuedConnection);
}

} // namespace Arkham
