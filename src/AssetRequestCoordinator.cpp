#include "AssetRequestCoordinator.h"

#include "AssetLocator.h"

#include <QMetaObject>
#include <QPointer>
#include <utility>

namespace Arkham {

namespace {

// A canonical, order-sensitive string identity for an AssetKey, used only
// to coalesce concurrent identical requests while one is in flight. Two
// requests coalesce if and only if every field compares equal -- this is
// deliberately simpler than (and independent of) AssetCache::cacheKeyFor(),
// which hashes a resolved *candidate* URL rather than the logical request.
//
// Each field is length-prefixed ("<charCount>:<field>") rather than
// joined with a plain separator character: `identifier` passes through
// AssetLocator's strict grammar, but `locale` is NOT similarly validated,
// so a delimiter-based join could let a locale value containing the
// delimiter make two genuinely different AssetKey values collide onto
// the same operation key. Length-prefixing makes the concatenation
// injective regardless of what characters any field contains, since each
// field's exact length is known before its content is read.
QString lengthPrefixed(const QString &field) {
  return QString::number(field.size()) + u':' + field;
}

QString canonicalOperationKey(const AssetKey &key) {
  return lengthPrefixed(key.assetBase.toString(QUrl::FullyEncoded)) +
         lengthPrefixed(QString::number(static_cast<int>(key.category))) +
         lengthPrefixed(key.identifier) +
         lengthPrefixed(QString::number(static_cast<int>(key.side))) +
         lengthPrefixed(key.locale) +
         lengthPrefixed(QString::number(static_cast<int>(key.format)));
}

AssetCache::CachedEntry
toCachedEntry(const AssetNetworkFetcher::FetchedAsset &asset) {
  AssetCache::CachedEntry entry;
  entry.encodedBytes = asset.encodedBytes;
  entry.decodedImage = asset.decodedImage;
  entry.contentType = asset.contentType;
  entry.dimensions = asset.dimensions;
  entry.sha256Hex = asset.sha256Hex;
  entry.etag = asset.etag;
  entry.lastModified = asset.lastModified;
  return entry;
}

} // namespace

AssetRequestCoordinator::AssetRequestCoordinator(AssetCache &cache,
                                                 AssetNetworkFetcher &fetcher,
                                                 QObject *parent)
    : QObject(parent), m_cache(cache), m_fetcher(fetcher) {}

AssetRequestCoordinator::~AssetRequestCoordinator() {
  // Abort every in-flight fetch without invoking any consumer's callback:
  // destruction must never deliver a stale completion. Consumers simply
  // never hear back, exactly as documented for a destroyed QObject seam.
  for (auto it = m_operations.begin(); it != m_operations.end(); ++it) {
    if (it.value().fetchHandle.isValid()) {
      m_fetcher.cancel(it.value().fetchHandle);
    }
  }
  m_operations.clear();
  m_handleToOperation.clear();
}

AssetRequestCoordinator::RequestHandle
AssetRequestCoordinator::request(const AssetKey &key, ResultCallback callback) {
  const AssetOutcome<QVector<AssetCandidate>> candidatesResult =
      AssetLocator::resolveCandidates(key);
  if (!candidatesResult) {
    return registerImmediateCompletion(
        key, std::move(callback),
        AssetOutcome<AssetCache::CachedEntry>(candidatesResult.error()));
  }

  const QVector<AssetCandidate> &candidates = *candidatesResult;

  // Serve directly from cache if ANY candidate (in fallback order) is
  // already known-good: this reflects a previously-resolved outcome (e.g.
  // "the localized image 404s, English succeeded") without repeating the
  // fallback probing every time.
  for (const AssetCandidate &candidate : candidates) {
    const QString cacheKey = AssetCache::cacheKeyFor(candidate.url);

    // A same-process memory hit was already validated during this
    // process's own lifetime: serve it with no network round trip at all.
    if (auto hit = m_cache.lookupMemory(cacheKey)) {
      return registerImmediateCompletion(
          key, std::move(callback),
          ensureDecoded(std::move(*hit), key.format, cacheKey));
    }

    if (auto hit = m_cache.lookupDisk(cacheKey)) {
      AssetCache::CachedEntry entry = *hit;
      if (entry.etag.isEmpty() && entry.lastModified.isEmpty()) {
        // Nothing to conditionally revalidate against: serve as-is,
        // exactly like a memory hit.
        return registerImmediateCompletion(
            key, std::move(callback),
            ensureDecoded(std::move(entry), key.format, cacheKey));
      }

      // A disk hit carrying validators is revalidated with a real
      // conditional GET (see startRevalidation()) rather than trusted
      // forever: this is the only production code path that actually
      // exercises AssetNetworkFetcher::ConditionalHeaders end-to-end.
      //
      // Coalesce with an already in-flight identical request (revalidation
      // or otherwise) before starting a new one: without this check, two
      // concurrent request() calls for the same AssetKey that both land on
      // this disk-hit-with-validators path would each issue their own
      // conditional GET, silently bypassing the coordinator's coalescing
      // guarantee.
      const QString opKey = canonicalOperationKey(key);
      if (const std::optional<quint64> existingOperationId =
              findInFlightOperation(opKey)) {
        const quint64 handleId = m_nextHandle++;
        Operation &existing = m_operations[*existingOperationId];
        existing.consumers.append(Consumer{handleId, std::move(callback)});
        m_handleToOperation.insert(handleId, *existingOperationId);
        return RequestHandle{handleId};
      }

      const quint64 handleId = m_nextHandle++;
      const quint64 operationId = m_nextOperationId++;
      Operation operation;
      operation.key = key;
      operation.candidates = QVector<AssetCandidate>{candidate};
      operation.candidateIndex = 0;
      operation.isRevalidation = true;
      operation.revalidationCacheKey = cacheKey;
      operation.staleEntry = std::move(entry);
      operation.consumers.append(Consumer{handleId, std::move(callback)});
      m_operations.insert(operationId, std::move(operation));
      m_handleToOperation.insert(handleId, operationId);

      startRevalidation(operationId);
      return RequestHandle{handleId};
    }
  }

  const QString opKey = canonicalOperationKey(key);
  const quint64 handleId = m_nextHandle++;

  // Coalesce with an already-in-flight identical request, if one exists.
  if (const std::optional<quint64> existingOperationId =
          findInFlightOperation(opKey)) {
    Operation &existing = m_operations[*existingOperationId];
    existing.consumers.append(Consumer{handleId, std::move(callback)});
    m_handleToOperation.insert(handleId, *existingOperationId);
    return RequestHandle{handleId};
  }

  const quint64 operationId = m_nextOperationId++;
  Operation operation;
  operation.key = key;
  operation.candidates = candidates;
  operation.candidateIndex = 0;
  operation.consumers.append(Consumer{handleId, std::move(callback)});
  m_operations.insert(operationId, std::move(operation));
  m_handleToOperation.insert(handleId, operationId);

  startCandidate(operationId);

  return RequestHandle{handleId};
}

std::optional<quint64>
AssetRequestCoordinator::findInFlightOperation(const QString &opKey) const {
  for (auto it = m_operations.begin(); it != m_operations.end(); ++it) {
    if (canonicalOperationKey(it.value().key) == opKey) {
      return it.key();
    }
  }
  return std::nullopt;
}

AssetOutcome<AssetCache::CachedEntry>
AssetRequestCoordinator::ensureDecoded(AssetCache::CachedEntry entry,
                                       AssetFormat format,
                                       const QString &cacheKey) {
  if (!entry.decodedImage.isNull()) {
    return AssetOutcome<AssetCache::CachedEntry>(std::move(entry));
  }

  const AssetOutcome<QImage> decoded =
      m_fetcher.decodeAndValidate(entry.encodedBytes, format);
  if (!decoded) {
    return AssetOutcome<AssetCache::CachedEntry>(decoded.error());
  }

  entry.decodedImage = *decoded;
  m_cache.updateMemoryDecodedImage(cacheKey, entry.decodedImage);
  return AssetOutcome<AssetCache::CachedEntry>(std::move(entry));
}

AssetRequestCoordinator::RequestHandle
AssetRequestCoordinator::registerImmediateCompletion(
    const AssetKey &key, ResultCallback callback,
    AssetOutcome<AssetCache::CachedEntry> result) {
  // Even an immediate (cache-hit or pre-network-error) completion is
  // routed through the normal operation/consumer bookkeeping and given a
  // real, valid handle: this is what lets cancel(handle) suppress a queued
  // completion that has not yet run, and is exactly what a QML seam's
  // destructor relies on when it calls cancel() unconditionally.
  const quint64 handleId = m_nextHandle++;
  const quint64 operationId = m_nextOperationId++;

  Operation operation;
  operation.key = key;
  operation.consumers.append(Consumer{handleId, std::move(callback)});
  m_operations.insert(operationId, std::move(operation));
  m_handleToOperation.insert(handleId, operationId);

  QPointer<AssetRequestCoordinator> self(this);
  QMetaObject::invokeMethod(
      this,
      [self, operationId, result = std::move(result)]() mutable {
        if (self) {
          self->completeOperation(operationId, std::move(result));
        }
      },
      Qt::QueuedConnection);

  return RequestHandle{handleId};
}

void AssetRequestCoordinator::startCandidate(quint64 operationId) {
  auto it = m_operations.find(operationId);
  if (it == m_operations.end()) {
    return;
  }
  Operation &operation = it.value();
  const AssetCandidate &candidate =
      operation.candidates[operation.candidateIndex];

  QPointer<AssetRequestCoordinator> self(this);
  operation.fetchHandle = m_fetcher.fetch(
      candidate.url, operation.key.format, {},
      [self, operationId](
          AssetOutcome<AssetNetworkFetcher::ConditionalFetchResult> result) {
        if (!self) {
          return;
        }
        auto opIt = self->m_operations.find(operationId);
        if (opIt == self->m_operations.end()) {
          return; // operation already fully cancelled/completed
        }

        if (!result) {
          const AssetError &error = result.error();
          if (error.code == AssetErrorCode::NotFound) {
            Operation &operation = opIt.value();
            if (operation.candidateIndex + 1 < operation.candidates.size()) {
              ++operation.candidateIndex;
              self->startCandidate(operationId);
              return;
            }
          }
          self->completeOperation(operationId,
                                  AssetOutcome<AssetCache::CachedEntry>(error));
          return;
        }

        // fetch() is always called here without conditional headers, so a
        // notModified==true result would itself be a protocol violation
        // already rejected as AssetErrorCode::ConditionalWithoutCachedBody
        // by AssetNetworkFetcher -- this branch is unreachable in practice
        // but handled defensively rather than dereferencing an empty
        // optional.
        if (result->notModified || !result->asset.has_value()) {
          self->completeOperation(
              operationId,
              AssetOutcome<AssetCache::CachedEntry>(AssetError{
                  AssetErrorCode::ConditionalWithoutCachedBody,
                  QStringLiteral("unexpected not-modified result for an "
                                 "unconditional fetch")}));
          return;
        }

        Operation &operation = opIt.value();
        const QString cacheKey = AssetCache::cacheKeyFor(
            operation.candidates[operation.candidateIndex].url);
        AssetCache::CachedEntry entry = toCachedEntry(*result->asset);
        self->m_cache.store(cacheKey, entry);
        self->completeOperation(operationId,
                                AssetOutcome<AssetCache::CachedEntry>(entry));
      });
}

void AssetRequestCoordinator::startRevalidation(quint64 operationId) {
  auto it = m_operations.find(operationId);
  if (it == m_operations.end()) {
    return;
  }
  Operation &operation = it.value();
  const AssetCandidate &candidate = operation.candidates.first();
  const AssetCache::CachedEntry &staleEntry = *operation.staleEntry;

  AssetNetworkFetcher::ConditionalHeaders conditional;
  conditional.etag = staleEntry.etag;
  conditional.lastModified = staleEntry.lastModified;

  QPointer<AssetRequestCoordinator> self(this);
  operation.fetchHandle = m_fetcher.fetch(
      candidate.url, operation.key.format, conditional,
      [self, operationId](
          AssetOutcome<AssetNetworkFetcher::ConditionalFetchResult> result) {
        if (!self) {
          return;
        }
        auto opIt = self->m_operations.find(operationId);
        if (opIt == self->m_operations.end()) {
          return; // operation already fully cancelled/completed
        }
        Operation &operation = opIt.value();
        const AssetCache::CachedEntry stale = *operation.staleEntry;

        // "Stale-if-error": ANY revalidation failure (a 404 because the
        // origin removed this exact candidate, a transport error, a
        // timeout, an integrity/codec failure, cancellation racing a
        // teardown, etc.) serves the still-valid stale cached entry
        // as-is rather than surfacing an error or advancing candidates --
        // a briefly-unreachable or since-changed origin can never make
        // previously cached, already-displayed art disappear.
        if (!result) {
          self->completeOperation(
              operationId, self->ensureDecoded(stale, operation.key.format,
                                               operation.revalidationCacheKey));
          return;
        }

        if (result->notModified) {
          // Confirmed unchanged: refresh only lastAccess (the payload
          // bytes are never touched), then serve the same stale entry.
          self->m_cache.touchAfterNotModified(operation.revalidationCacheKey,
                                              QString(), QString());
          self->completeOperation(
              operationId, self->ensureDecoded(stale, operation.key.format,
                                               operation.revalidationCacheKey));
          return;
        }

        if (!result->asset.has_value()) {
          // Defensive only: AssetNetworkFetcher never returns
          // notModified==false with an empty asset.
          self->completeOperation(
              operationId, self->ensureDecoded(stale, operation.key.format,
                                               operation.revalidationCacheKey));
          return;
        }

        // The origin sent a fresh 200 body despite our conditional
        // headers (its content genuinely changed): replace the cached
        // entry and serve the new content.
        AssetCache::CachedEntry fresh = toCachedEntry(*result->asset);
        self->m_cache.store(operation.revalidationCacheKey, fresh);
        self->completeOperation(operationId,
                                AssetOutcome<AssetCache::CachedEntry>(fresh));
      });
}

void AssetRequestCoordinator::completeOperation(
    quint64 operationId, AssetOutcome<AssetCache::CachedEntry> result) {
  auto it = m_operations.find(operationId);
  if (it == m_operations.end()) {
    return;
  }
  Operation operation = std::move(it.value());
  m_operations.erase(it);
  for (const Consumer &consumer : operation.consumers) {
    m_handleToOperation.remove(consumer.handleId);
  }
  dispatchToConsumers(operation, std::move(result));
}

void AssetRequestCoordinator::dispatchToConsumers(
    Operation &operation, AssetOutcome<AssetCache::CachedEntry> result) {
  QPointer<AssetRequestCoordinator> self(this);
  for (Consumer &consumer : operation.consumers) {
    QMetaObject::invokeMethod(
        this,
        [self, callback = std::move(consumer.callback), result]() mutable {
          if (self) {
            // `result` is this lambda's own private capture (a per-
            // consumer copy of the shared outcome, made because there
            // may be multiple consumers to fan out to) -- once we are
            // inside this specific lambda invocation, nothing else still
            // needs it, so move it into the callback rather than paying
            // for another deep copy of a potentially large CachedEntry
            // (encoded bytes plus a decoded QImage).
            std::move(callback)(std::move(result));
          }
        },
        Qt::QueuedConnection);
  }
}

void AssetRequestCoordinator::cancel(RequestHandle handle) {
  if (!handle.isValid()) {
    return;
  }
  auto handleIt = m_handleToOperation.find(handle.id);
  if (handleIt == m_handleToOperation.end()) {
    return; // stale or already-completed handle: safe no-op
  }
  const quint64 operationId = handleIt.value();
  m_handleToOperation.erase(handleIt);

  auto opIt = m_operations.find(operationId);
  if (opIt == m_operations.end()) {
    return;
  }
  Operation &operation = opIt.value();

  int removedIndex = -1;
  for (int i = 0; i < operation.consumers.size(); ++i) {
    if (operation.consumers[i].handleId == handle.id) {
      removedIndex = i;
      break;
    }
  }
  if (removedIndex < 0) {
    return;
  }

  ResultCallback cancelledCallback =
      std::move(operation.consumers[removedIndex].callback);
  operation.consumers.remove(removedIndex);

  QPointer<AssetRequestCoordinator> self(this);
  QMetaObject::invokeMethod(
      this,
      [self, callback = std::move(cancelledCallback)]() mutable {
        if (self) {
          std::move(callback)(AssetOutcome<AssetCache::CachedEntry>(
              AssetError{AssetErrorCode::Cancelled,
                         QStringLiteral("request was cancelled")}));
        }
      },
      Qt::QueuedConnection);

  if (operation.consumers.isEmpty()) {
    // Last consumer gone: actually abort the underlying fetch. Remove the
    // operation from the map FIRST so a reply that was already in flight
    // (racing with this cancellation) can never find and complete it.
    AssetNetworkFetcher::FetchHandle fetchHandle = operation.fetchHandle;
    m_operations.erase(opIt);
    if (fetchHandle.isValid()) {
      m_fetcher.cancel(fetchHandle);
    }
  }
}

} // namespace Arkham
