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

// ValidatedAssetSource::fromRaw() already performed the one-and-only
// normalisation this base URL will ever receive (see AssetTypes.h/.cpp) --
// there is no separate re-normalisation step left to duplicate here, and
// no raw string left to fall back to for an invalid base: an
// AssetKey::assetBase that is not isValid() carries no useful distinguishing
// information anyway (AssetLocator::resolveCandidates() rejects it
// identically regardless of how it became invalid), so every such
// operation deliberately coalesces onto the same fixed sentinel string.
QString canonicalizedAssetBase(const ValidatedAssetSource &assetBase) {
  if (!assetBase.isValid()) {
    return QStringLiteral("<invalid>");
  }
  return assetBase.normalizedUrl().toString(QUrl::FullyEncoded);
}

QString canonicalOperationKey(const AssetKey &key) {
  return lengthPrefixed(canonicalizedAssetBase(key.assetBase)) +
         lengthPrefixed(QString::number(static_cast<int>(key.category))) +
         lengthPrefixed(key.identifier) +
         lengthPrefixed(QString::number(static_cast<int>(key.side))) +
         lengthPrefixed(key.locale) + lengthPrefixed(key.homebrewNamespace) +
         lengthPrefixed(key.mutationId) +
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

  // Walk candidates in STRICT priority order (review item 5): a
  // lower-priority candidate that happens to already be cached must
  // never be served ahead of a higher-priority candidate that has not
  // yet been tried at all. The only thing that authorizes skipping a
  // candidate here without trying it is an exact, authoritative
  // negative-404 record for that EXACT resolved candidate URL (see
  // hasNegative404() / the class comment) -- anything else (no record at
  // all) stops this loop immediately, even if a later candidate would
  // otherwise be servable straight from cache.
  int candidateIndex = 0;
  for (; candidateIndex < candidates.size(); ++candidateIndex) {
    const AssetCandidate &candidate = candidates[candidateIndex];
    const QString cacheKey = AssetCache::cacheKeyFor(candidate.url);

    if (hasNegative404(cacheKey)) {
      continue; // authoritatively confirmed absent: try the next candidate
    }

    // A same-process memory hit was already validated during this
    // process's own lifetime: serve it with no network round trip at all.
    // Routed through registerCacheHitCompletion() (review item 9), not
    // ensureDecoded() + registerImmediateCompletion() directly: a
    // decode/integrity failure discovered here must be able to quarantine
    // this exact entry and retry the SAME candidate over the network
    // rather than permanently poisoning this key with a fixed, pre-
    // computed failure outcome.
    if (auto hit = m_cache.lookupMemory(cacheKey)) {
      return registerCacheHitCompletion(key, std::move(callback), candidates,
                                        candidateIndex, std::move(*hit),
                                        cacheKey);
    }

    if (auto hit = m_cache.lookupDisk(cacheKey)) {
      AssetCache::CachedEntry entry = *hit;
      if (entry.etag.isEmpty() && entry.lastModified.isEmpty()) {
        // Nothing to conditionally revalidate against: serve as-is,
        // exactly like a memory hit (see the comment above).
        return registerCacheHitCompletion(key, std::move(callback), candidates,
                                          candidateIndex, std::move(entry),
                                          cacheKey);
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
      // The full candidate vector (not just this one candidate) is kept
      // so that if this revalidation itself receives a definitive 404,
      // it can evict this entry and advance through the REMAINING
      // candidates exactly like a first-time miss (see
      // startRevalidation()), rather than dead-ending here.
      operation.candidates = candidates;
      operation.candidateIndex = candidateIndex;
      operation.isRevalidation = true;
      operation.revalidationCacheKey = cacheKey;
      operation.staleEntry = std::move(entry);
      operation.consumers.append(Consumer{handleId, std::move(callback)});
      m_operations.insert(operationId, std::move(operation));
      m_handleToOperation.insert(handleId, operationId);

      startRevalidation(operationId);
      return RequestHandle{handleId};
    }

    // Neither a confirmed-absent record nor a cache hit: this is the
    // first genuinely untried candidate in strict priority order. Stop
    // probing the cache here and fall through to the network below --
    // an untried higher-priority candidate must never be skipped in
    // favour of a lower-priority one that merely happens to already be
    // cached.
    break;
  }

  if (candidateIndex >= candidates.size()) {
    // Every remaining candidate carries an authoritative confirmed-404
    // record: the whole logical request is definitively not-found, with
    // no network round trip required at all.
    return registerImmediateCompletion(
        key, std::move(callback),
        AssetOutcome<AssetCache::CachedEntry>(AssetError{
            AssetErrorCode::NotFound,
            QStringLiteral("every candidate previously confirmed absent "
                           "(negative cache)")}));
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
  operation.candidateIndex = candidateIndex;
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

bool AssetRequestCoordinator::hasNegative404(const QString &cacheKey) const {
  return m_negative404.contains(cacheKey);
}

void AssetRequestCoordinator::recordNegative404(const QString &cacheKey) {
  m_negative404.insert(cacheKey);
}

void AssetRequestCoordinator::clearNegative404(const QString &cacheKey) {
  m_negative404.remove(cacheKey);
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

AssetRequestCoordinator::RequestHandle
AssetRequestCoordinator::registerCacheHitCompletion(
    const AssetKey &key, ResultCallback callback,
    QVector<AssetCandidate> candidates, int candidateIndex,
    AssetCache::CachedEntry entry, QString cacheKey) {
  const quint64 handleId = m_nextHandle++;
  const quint64 operationId = m_nextOperationId++;

  Operation operation;
  operation.key = key;
  // The full candidate vector and this exact index are kept (not just
  // discarded after the cache hit) so that a quarantine discovered
  // inside completeCacheReadOrQuarantine() below can retry precisely
  // this SAME candidate via the ordinary startCandidate() network path,
  // exactly like a first-time miss would -- see that method's comment.
  operation.candidates = std::move(candidates);
  operation.candidateIndex = candidateIndex;
  operation.consumers.append(Consumer{handleId, std::move(callback)});
  m_operations.insert(operationId, std::move(operation));
  m_handleToOperation.insert(handleId, operationId);

  QPointer<AssetRequestCoordinator> self(this);
  QMetaObject::invokeMethod(
      this,
      [self, operationId, entry = std::move(entry),
       cacheKey = std::move(cacheKey)]() mutable {
        if (self) {
          self->completeCacheReadOrQuarantine(operationId, std::move(entry),
                                              cacheKey,
                                              /*promoteOnSuccess=*/false);
        }
      },
      Qt::QueuedConnection);

  return RequestHandle{handleId};
}

bool AssetRequestCoordinator::isQuarantineWorthy(AssetErrorCode code) {
  switch (code) {
  case AssetErrorCode::MagicBytesMismatch:
  case AssetErrorCode::MalformedImage:
  case AssetErrorCode::DimensionTooLarge:
  case AssetErrorCode::PixelBudgetExceeded:
  case AssetErrorCode::CacheCorrupt:
    return true;
  default:
    // In particular, AssetErrorCode::UnsupportedCodec is deliberately
    // excluded: the bytes are still perfectly valid, this process simply
    // has no installed decoder for them right now (see the header
    // comment) -- never quarantine/delete valid bytes for that reason.
    return false;
  }
}

void AssetRequestCoordinator::completeCacheReadOrQuarantine(
    quint64 operationId, AssetCache::CachedEntry entry, const QString &cacheKey,
    bool promoteOnSuccess) {
  auto it = m_operations.find(operationId);
  if (it == m_operations.end()) {
    return; // cancelled/destroyed before this queued call ran
  }
  Operation &operation = it.value();
  AssetOutcome<AssetCache::CachedEntry> outcome =
      ensureDecoded(std::move(entry), operation.key.format, cacheKey);

  if (outcome) {
    if (promoteOnSuccess) {
      m_cache.promoteToMemory(cacheKey, *outcome);
    }
    completeOperation(operationId, std::move(outcome));
    return;
  }

  if (!isQuarantineWorthy(outcome.error().code)) {
    completeOperation(operationId, std::move(outcome));
    return;
  }

  // Review item 9: this exact cached generation just failed a format/
  // magic/decode/dimension/pixel-budget re-check against CURRENT limits
  // -- it must never be served again as-is, and never silently keep
  // failing every future request for the same key forever. Evict both
  // the memory and disk state for this exact resolved-candidate cache
  // key, then retry precisely the SAME candidate as a genuine network
  // miss. isRevalidation/staleEntry are reset first so the retry goes
  // through the ordinary unconditional-fetch path (startCandidate()),
  // never back into startRevalidation() for bytes that no longer exist
  // anywhere. A decode failure on the freshly-refetched bytes (if any)
  // completes via startCandidate()'s own normal fetch-failure handling,
  // which never re-enters this method -- so a bad origin resource can
  // fail at most once more, never loop.
  m_cache.invalidate(cacheKey);
  operation.isRevalidation = false;
  operation.staleEntry.reset();
  startCandidate(operationId);
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
            // Authoritative: this EXACT candidate is definitively absent.
            // Record it so a future request() for a different logical
            // key that also resolves to this same candidate can skip it
            // outright (see hasNegative404() / the class comment) --
            // never recorded for any other error code.
            self->recordNegative404(AssetCache::cacheKeyFor(
                operation.candidates[operation.candidateIndex].url));
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
        // Defensive: a candidate that once 404'd could, in principle,
        // reappear -- never leave a stale negative record pointing at a
        // now-confirmed-good candidate.
        self->clearNegative404(cacheKey);
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
  const AssetCandidate &candidate =
      operation.candidates[operation.candidateIndex];
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

        // A definitive 404 is the ONE revalidation failure that does NOT
        // fall back to "stale-if-error" (review item 5): the origin has
        // authoritatively confirmed this exact candidate no longer
        // exists, so the stale cached entry is evicted (never served as
        // a false "still good" success), a negative-404 record is
        // written for it, and the request advances through the
        // remaining candidates exactly like a first-time miss would --
        // reusing the ordinary startCandidate() fetch path from here on.
        if (!result && result.error().code == AssetErrorCode::NotFound) {
          self->m_cache.invalidate(operation.revalidationCacheKey);
          self->recordNegative404(operation.revalidationCacheKey);
          if (operation.candidateIndex + 1 < operation.candidates.size()) {
            ++operation.candidateIndex;
            operation.isRevalidation = false;
            operation.staleEntry.reset();
            self->startCandidate(operationId);
            return;
          }
          self->completeOperation(
              operationId,
              AssetOutcome<AssetCache::CachedEntry>(result.error()));
          return;
        }

        // "Stale-if-error": every OTHER revalidation failure (transport
        // error, timeout, TLS failure, 5xx, an integrity/codec failure,
        // cancellation racing a teardown, etc.) attempts to serve the
        // still-valid stale cached entry rather than surfacing the
        // *revalidation's* error or advancing candidates -- a briefly-
        // unreachable or since-changed origin can never make previously
        // cached, already-displayed art disappear, and none of these
        // outcomes is proof the resource is actually gone. This is
        // "as-is" only in the sense that the cached payload bytes are
        // never re-fetched or rewritten by THIS revalidation attempt;
        // completeCacheReadOrQuarantine() below can still discover its
        // own independent, genuine failure of the cached entry itself
        // (e.g. the on-disk bytes no longer decode against current
        // limits/codec support) and, for a quarantine-worthy one (review
        // item 9), evict this entry and retry the same candidate as a
        // fresh network miss instead of serving the same doomed stale
        // bytes forever.
        if (!result) {
          self->completeCacheReadOrQuarantine(operationId, stale,
                                              operation.revalidationCacheKey,
                                              /*promoteOnSuccess=*/false);
          return;
        }

        if (result->notModified) {
          // Confirmed unchanged: refresh lastAccess, and adopt any
          // validator the 304 itself refreshed (RFC 7232 S4.1 allows a
          // 304 to rotate ETag/Last-Modified without a body); the payload
          // bytes are never touched. Empty fields leave the existing
          // validator untouched (see touchAfterNotModified()).
          self->m_cache.touchAfterNotModified(operation.revalidationCacheKey,
                                              result->refreshedEtag,
                                              result->refreshedLastModified);
          // A validator-carrying disk hit is normally withheld from
          // memory promotion (see lookupDisk()) until it has actually
          // been revalidated -- this 304 IS that revalidation, so
          // completeCacheReadOrQuarantine() is asked to unconditionally
          // promote the (now-decoded) entry on success, letting a
          // subsequent same-process request for the same key
          // short-circuit via lookupMemory() instead of repeating the
          // conditional GET. On a quarantine-worthy decode failure
          // instead (review item 9), the just-touched metadata/payload
          // are evicted again and the same candidate is retried fresh.
          self->completeCacheReadOrQuarantine(operationId, stale,
                                              operation.revalidationCacheKey,
                                              /*promoteOnSuccess=*/true);
          return;
        }

        if (!result->asset.has_value()) {
          // Defensive only: AssetNetworkFetcher never returns
          // notModified==false with an empty asset.
          self->completeCacheReadOrQuarantine(operationId, stale,
                                              operation.revalidationCacheKey,
                                              /*promoteOnSuccess=*/false);
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
  // The consumer handle must remain valid for cancel() until the queued
  // delivery below actually runs (see the class comment): simply erasing
  // it from m_handleToOperation here, with nothing replacing it, would
  // let a cancel() call racing this completion see an indistinguishable
  // "stale" handle and silently no-op even though no delivery has
  // happened yet. Instead, hand each consumer's handle off to a lighter
  // "delivery pending" registry that cancel() also consults; each
  // consumer's queued lambda in dispatchToConsumers() below consumes
  // (erases) its own entry exactly when it runs.
  for (const Consumer &consumer : operation.consumers) {
    m_handleToOperation.remove(consumer.handleId);
    m_pendingDeliveryCancelled.insert(consumer.handleId,
                                      std::make_shared<bool>(false));
  }
  dispatchToConsumers(operation, std::move(result));
}

void AssetRequestCoordinator::dispatchToConsumers(
    Operation &operation, AssetOutcome<AssetCache::CachedEntry> result) {
  QPointer<AssetRequestCoordinator> self(this);
  // Copilot review (round 32, suppressed comment): share the single
  // completion result across every fanned-out consumer via one
  // shared_ptr rather than materialising a fresh captured copy of
  // `result` inside each consumer's own closure below. Every member of
  // AssetOutcome<CachedEntry> (QByteArray/QImage/QString) is an
  // implicitly-shared, copy-on-write Qt value type, so even the
  // previous per-consumer captured copies were already cheap reference
  // bumps rather than genuine deep duplication of encoded bytes or
  // decoded pixel data -- but sharing one instance still avoids N
  // redundant std::optional<CachedEntry>/AssetError wrapper copies (and
  // their own reference-count bookkeeping) when an operation fans out
  // to many coalesced consumers.
  auto sharedResult =
      std::make_shared<const AssetOutcome<AssetCache::CachedEntry>>(
          std::move(result));
  for (Consumer &consumer : operation.consumers) {
    const quint64 handleId = consumer.handleId;
    QMetaObject::invokeMethod(
        this,
        [self, handleId, callback = std::move(consumer.callback),
         sharedResult]() mutable {
          if (!self) {
            return;
          }
          // A cancel() call that raced this queued delivery (arriving
          // after completeOperation() ran but before this exact lambda
          // did) flips the shared flag below rather than silently
          // no-op-ing on a handle it can no longer find in
          // m_handleToOperation; honour that here by substituting
          // Cancelled for the real result, so cancel()'s "own callback
          // is invoked exactly once, with Cancelled" contract holds
          // across this window too.
          bool cancelled = false;
          auto flagIt = self->m_pendingDeliveryCancelled.find(handleId);
          if (flagIt != self->m_pendingDeliveryCancelled.end()) {
            cancelled = *flagIt.value();
            self->m_pendingDeliveryCancelled.erase(flagIt);
          }
          if (cancelled) {
            std::move(callback)(AssetOutcome<AssetCache::CachedEntry>(
                AssetError{AssetErrorCode::Cancelled,
                           QStringLiteral("request was cancelled")}));
            return;
          }
          // `sharedResult` is shared across every consumer's closure
          // (some of which may not have run yet), so it can never be
          // moved-from here -- copy-construct the callback's argument
          // from the shared const instance instead. As above, this is
          // cheap (reference-count bumps on implicitly-shared Qt
          // members), not a deep copy.
          std::move(callback)(*sharedResult);
        },
        Qt::QueuedConnection);
  }
}

void AssetRequestCoordinator::cancel(RequestHandle handle) {
  if (!handle.isValid()) {
    return;
  }

  // Check the "delivery already dispatched but not yet run" registry
  // FIRST: completeOperation() moves a consumer's handle here the
  // instant it runs, well before this consumer's own queued delivery
  // actually executes, so a handle can legitimately be absent from
  // m_handleToOperation below yet still be cancel()-able.
  auto pendingIt = m_pendingDeliveryCancelled.find(handle.id);
  if (pendingIt != m_pendingDeliveryCancelled.end()) {
    *pendingIt.value() = true;
    return;
  }

  auto handleIt = m_handleToOperation.find(handle.id);
  if (handleIt == m_handleToOperation.end()) {
    return; // stale or already-delivered handle: safe no-op
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
