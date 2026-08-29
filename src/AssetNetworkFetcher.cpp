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
    for (qint64 offset = 8; offset + 4 <= boxEnd; offset += 4) {
      const QByteArray brand = bytes.mid(offset, 4);
      if (brand == QByteArrayLiteral("avif") ||
          brand == QByteArrayLiteral("avis")) {
        return AssetFormat::Avif;
      }
    }
  }
  return std::nullopt;
}

// Normalises a Content-Type header value to a bare, lowercase media type
// (e.g. "image/png; charset=binary" -> "image/png"), so a parameter suffix
// cannot defeat the comparison in either direction.
QString normalizeContentType(const QString &raw) {
  const qsizetype semi = raw.indexOf(u';');
  const QString mediaType = (semi >= 0) ? raw.left(semi) : raw;
  return mediaType.trimmed().toLower();
}

const char *qtImageFormatName(AssetFormat format) {
  switch (format) {
  case AssetFormat::Avif:
    return "avif";
  case AssetFormat::Jpeg:
    return "jpg";
  case AssetFormat::Png:
    return "png";
  }
  Q_UNREACHABLE_RETURN("");
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

  // Drain any final buffered bytes not yet delivered via readyRead.
  pendingCopy.buffer.append(reply->readAll());
  if (pendingCopy.buffer.size() > m_limits.maxEncodedBytes) {
    emitResult(AssetError{AssetErrorCode::ResponseTooLarge,
                          QStringLiteral("response body exceeded the "
                                         "configured encoded-size limit")});
    return;
  }

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

  const std::optional<AssetFormat> sniffed = sniffMagicBytes(body);
  if (!sniffed || *sniffed != pendingCopy.expectedFormat) {
    emitResult(AssetError{
        AssetErrorCode::MagicBytesMismatch,
        QStringLiteral("response body's magic bytes do not match the "
                       "expected asset format")});
    return;
  }

  QBuffer buffer;
  buffer.setData(body);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer, qtImageFormatName(pendingCopy.expectedFormat));

  if (!QImageReader::supportedImageFormats().contains(
          QByteArray(qtImageFormatName(pendingCopy.expectedFormat)))) {
    emitResult(AssetError{
        AssetErrorCode::UnsupportedCodec,
        QStringLiteral("no installed Qt image plugin can decode this "
                       "asset's format")});
    return;
  }

  const QSize declaredSize = reader.size();
  if (!declaredSize.isValid() || declaredSize.width() <= 0 ||
      declaredSize.height() <= 0) {
    emitResult(AssetError{AssetErrorCode::MalformedImage,
                          QStringLiteral("image header is malformed or "
                                         "reports non-positive dimensions")});
    return;
  }
  if (declaredSize.width() > m_limits.maxDimensionPixels ||
      declaredSize.height() > m_limits.maxDimensionPixels) {
    emitResult(AssetError{
        AssetErrorCode::DimensionTooLarge,
        QStringLiteral("image dimension %1x%2 exceeds the configured cap "
                       "of %3 pixels per side")
            .arg(declaredSize.width())
            .arg(declaredSize.height())
            .arg(m_limits.maxDimensionPixels)});
    return;
  }
  // 64-bit multiplication: both operands are already bounded above by
  // maxDimensionPixels (a configurable but always-small int), so this can
  // never overflow even at the largest permitted configuration.
  const qint64 totalPixels =
      static_cast<qint64>(declaredSize.width()) * declaredSize.height();
  if (totalPixels > m_limits.maxTotalPixels) {
    emitResult(AssetError{
        AssetErrorCode::PixelBudgetExceeded,
        QStringLiteral("image totals %1 pixels, exceeding the configured "
                       "cap of %2")
            .arg(totalPixels)
            .arg(m_limits.maxTotalPixels)});
    return;
  }

  QImage decoded = reader.read();
  if (decoded.isNull()) {
    if (reader.error() == QImageReader::UnsupportedFormatError) {
      emitResult(AssetError{
          AssetErrorCode::UnsupportedCodec,
          QStringLiteral("installed Qt image plugin declined to decode "
                         "this asset")});
    } else {
      emitResult(AssetError{AssetErrorCode::MalformedImage,
                            QStringLiteral("image body failed to decode: %1")
                                .arg(reader.errorString())});
    }
    return;
  }

  FetchedAsset asset;
  asset.encodedBytes = body;
  asset.contentType = declaredContentType;
  asset.dimensions = declaredSize;
  asset.decodedImage = std::move(decoded);
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
