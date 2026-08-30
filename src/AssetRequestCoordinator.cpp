#include "AssetRequestCoordinator.h"

#include "AssetLocator.h"

#include <QMetaObject>
#include <QPointer>
#include <chrono>
#include <utility>

namespace Arkham {

namespace {

// Review round-3 item 13: the production default for
// AssetRequestCoordinator::m_monotonicNowMs. std::chrono::steady_clock
// (never system_clock/QDateTime) is used deliberately: negative-404 TTL
// expiry must never be sensitive to a wall-clock adjustment, NTP step, or
// timezone/DST change, forward or backward.
qint64 defaultMonotonicNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

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
  // Review round-3 fresh-review item: this must cover EVERY field
  // AssetKey::operator== compares, or two genuinely different requests
  // (identical identifier/side but a different back identity) can
  // incorrectly coalesce onto the same in-flight operation and one
  // consumer would receive the wrong candidate/result. backKind is
  // std::nullopt for every non-Back side (see AssetKey's own field
  // documentation); it is mapped to a "none" sentinel string (never
  // producible by QString::number() of an enum's underlying int) before
  // being length-prefixed the same uniform way as every other field
  // here, so its presence/absence is unambiguous without a separate
  // encoding scheme.
  const QString backKindString =
      key.backKind.has_value()
          ? QString::number(static_cast<int>(*key.backKind))
          : QStringLiteral("none");
  return lengthPrefixed(canonicalizedAssetBase(key.assetBase)) +
         lengthPrefixed(QString::number(static_cast<int>(key.category))) +
         lengthPrefixed(key.identifier) +
         lengthPrefixed(QString::number(static_cast<int>(key.side))) +
         lengthPrefixed(key.locale) + lengthPrefixed(key.homebrewNamespace) +
         lengthPrefixed(key.mutationId) + lengthPrefixed(backKindString) +
         lengthPrefixed(key.otherSideIdentifier) +
         lengthPrefixed(key.customBackFilename) +
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
    : QObject(parent), m_cache(cache), m_fetcher(fetcher),
      m_monotonicNowMs(&defaultMonotonicNowMs) {}

AssetRequestCoordinator::~AssetRequestCoordinator() {
  // Abort every in-flight fetch without invoking any consumer's callback:
  // destruction must never deliver a stale completion. Consumers simply
  // never hear back, exactly as documented for a destroyed QObject seam.
  // Round-6 item 8: a shared CandidateAttempt's fetchHandle is copied
  // onto every one of its subscribing Operations, so the same handle
  // value may be cancelled here more than once -- AssetNetworkFetcher::
  // cancel() on an already-cancelled/unknown handle is documented as a
  // safe no-op, so this remains correct without any special-casing.
  for (auto it = m_operations.begin(); it != m_operations.end(); ++it) {
    if (it.value().fetchHandle.isValid()) {
      m_fetcher.cancel(it.value().fetchHandle);
    }
  }
  m_operations.clear();
  m_handleToOperation.clear();
  m_candidateAttempts.clear();
}

AssetRequestCoordinator::RequestHandle
AssetRequestCoordinator::request(const AssetKey &key, ResultCallback callback) {
  // Review round-4 item 6: join an already in-flight operation for the
  // EXACT SAME canonicalized AssetKey (see canonicalOperationKey()) FIRST
  // -- strictly before any cache lookup (memory or disk) is even
  // attempted, and strictly before AssetLocator::resolveCandidates() is
  // called again for this key. Previously, coalescing was only checked
  // on the revalidation and network-fetch paths below; a plain cache hit
  // (memory OR disk-with-no-validators) instead unconditionally created
  // its OWN brand-new Operation and queued its OWN independent
  // ensureDecoded() call (see registerCacheHitCompletion()) for every
  // single request() call, even when several concurrent calls named the
  // exact same AssetKey and would clearly resolve to the exact same
  // cached bytes. Two (or a burst of many) concurrent QML-driven
  // requests for the same still-image asset -- e.g. the same card's art
  // requested by several simultaneously-visible UI elements before the
  // first request's queued completion has even run -- would therefore
  // each independently perform a full, expensive decode of the same
  // near-32-megapixel image in parallel, multiplying peak memory/CPU
  // cost by the number of concurrent callers instead of doing the work
  // once and fanning the single result out to every consumer. Checking
  // here, before ANY of that work begins, means every one of those
  // concurrent calls (regardless of whether the eventual path turns out
  // to be a memory hit, a disk hit, a conditional revalidation, or a
  // fresh network fetch) shares exactly one Operation and therefore
  // exactly one decode/fetch, with every consumer's callback appended to
  // that same operation's consumer list (see completeOperation()).
  const QString opKey = canonicalOperationKey(key);
  if (const std::optional<quint64> existingOperationId =
          findInFlightOperation(opKey)) {
    const quint64 handleId = m_nextHandle++;
    Operation &existing = m_operations[*existingOperationId];
    existing.consumers.append(Consumer{handleId, std::move(callback)});
    m_handleToOperation.insert(handleId, *existingOperationId);
    return RequestHandle{handleId};
  }

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
      // Review round-3 item 14: mint a fresh ISSUANCE value for this
      // cache hit (never merely read the current applied watermark --
      // see issueCacheKeyGeneration()'s comment) so a concurrent network
      // fetch issued after this exact moment for the same cache key
      // correctly outranks this cache hit's own eventual decode/promote
      // side effects, regardless of which one finishes first.
      return registerCacheHitCompletion(
          key, std::move(callback), candidates, candidateIndex, std::move(*hit),
          cacheKey, issueCacheKeyGeneration(cacheKey));
    }

    if (auto hit = m_cache.lookupDisk(cacheKey)) {
      AssetCache::CachedEntry entry = *hit;
      if (entry.etag.isEmpty() && entry.lastModified.isEmpty()) {
        // Nothing to conditionally revalidate against: serve as-is,
        // exactly like a memory hit (see the comment above).
        // Review round-3 item 14: same reasoning as the memory-hit branch
        // above -- mint, never merely read.
        return registerCacheHitCompletion(
            key, std::move(callback), candidates, candidateIndex,
            std::move(entry), cacheKey, issueCacheKeyGeneration(cacheKey));
      }

      // A disk hit carrying validators is revalidated with a real
      // conditional GET (see startRevalidation()) rather than trusted
      // forever: this is the only production code path that actually
      // exercises AssetNetworkFetcher::ConditionalHeaders end-to-end.
      //
      // No coalescing check is needed here: request() already confirmed,
      // strictly before this cache lookup ran at all, that no operation
      // for this exact opKey is currently in flight (see the top of this
      // function, review round-4 item 6) -- nothing synchronous between
      // that check and here can have started one.
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

  const quint64 handleId = m_nextHandle++;
  // No coalescing check is needed here either -- see the top of this
  // function (review round-4 item 6): `opKey` was already confirmed not
  // in flight before any of the cache-lookup loop above ever ran.

  const quint64 operationId = m_nextOperationId++;
  Operation operation;
  operation.key = key;
  operation.candidates = candidates;
  operation.candidateIndex = candidateIndex;
  operation.consumers.append(Consumer{handleId, std::move(callback)});
  m_operations.insert(operationId, std::move(operation));
  m_handleToOperation.insert(handleId, operationId);

  // Review round-3 item 12: route the very first network-bound candidate
  // through the SAME single path every later 404/quarantine-driven
  // advance also uses (see advanceCandidates()'s comment) -- this
  // specific index was already independently confirmed above to be
  // neither negative-cached nor a cache hit, so advanceCandidates() will
  // immediately re-confirm that (a harmless, cheap re-check) and fall
  // through to startCandidate() exactly as before.
  advanceCandidates(operationId);

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

QString AssetRequestCoordinator::candidateAttemptKey(
    const QString &cacheKey, AssetFormat format, const QString &etag,
    const QString &lastModified) {
  // Round-6 item 8: separators cannot collide with any of the input
  // fields -- cacheKey is a fixed-length hex digest (see
  // AssetCache::cacheKeyFor()), assetFormatExtension() returns a fixed
  // short lowercase token from a closed enum, and etag/lastModified are
  // arbitrary but delimited unambiguously via QByteArray-style length
  // prefixes so an etag containing "|" can never be confused with a
  // field boundary.
  return cacheKey + QStringLiteral("|") + assetFormatExtension(format) +
         QStringLiteral("|e") + QString::number(etag.size()) +
         QStringLiteral(":") + etag + QStringLiteral("|m") +
         QString::number(lastModified.size()) + QStringLiteral(":") +
         lastModified;
}

AssetNetworkFetcher::FetchHandle
AssetRequestCoordinator::unsubscribeFromCandidateAttempt(
    quint64 operationId, const QString &attemptKey) {
  if (attemptKey.isEmpty()) {
    return AssetNetworkFetcher::FetchHandle{};
  }
  auto attemptIt = m_candidateAttempts.find(attemptKey);
  if (attemptIt == m_candidateAttempts.end()) {
    // Round-6 item 8: the attempt already fired (or was never actually
    // registered -- defensive only) between this operation being marked
    // pending and this call; nothing left to unsubscribe from, and the
    // shared fetch (if any) is either already completing or already
    // gone.
    return AssetNetworkFetcher::FetchHandle{};
  }
  attemptIt->subscriberOperationIds.removeAll(operationId);
  if (!attemptIt->subscriberOperationIds.isEmpty()) {
    // Round-6 item 8: at least one OTHER operation still needs this
    // shared fetch's eventual result -- the underlying HTTP request must
    // keep running for their sake, never aborted merely because this one
    // subscriber cancelled.
    return AssetNetworkFetcher::FetchHandle{};
  }
  const AssetNetworkFetcher::FetchHandle fetchHandle = attemptIt->fetchHandle;
  m_candidateAttempts.erase(attemptIt);
  return fetchHandle;
}

bool AssetRequestCoordinator::hasNegative404(const QString &cacheKey) const {
  const auto it = m_negative404.constFind(cacheKey);
  if (it == m_negative404.constEnd()) {
    return false;
  }
  // Review round-3 item 13: a record is authoritative only while BOTH
  // hold: (a) `cacheKey`'s applied generation has not moved past the
  // exact issuance this record was written under (a later success, or a
  // later negative recording -- from this operation or a completely
  // different one -- both count), and (b) its TTL has not yet elapsed
  // against the injectable monotonic clock. Lazy expiry: an
  // expired/superseded entry is simply left in place (never actively
  // swept) until overwritten by a fresh recordNegative404() or removed by
  // clearNegative404() -- this method never mutates state, consistent
  // with it being callable from a const context.
  return it->generation == currentCacheKeyGeneration(cacheKey) &&
         m_monotonicNowMs() < it->expiresAtMonotonicMs;
}

void AssetRequestCoordinator::recordNegative404(const QString &cacheKey,
                                                quint64 generation) {
  m_negative404.insert(
      cacheKey,
      Negative404Entry{generation, m_monotonicNowMs() + kNegative404TtlMs});
}

void AssetRequestCoordinator::clearNegative404(const QString &cacheKey) {
  m_negative404.remove(cacheKey);
}

quint64 AssetRequestCoordinator::currentCacheKeyGeneration(
    const QString &cacheKey) const {
  return m_cacheKeyGeneration.value(cacheKey, 0);
}

quint64
AssetRequestCoordinator::issueCacheKeyGeneration(const QString &cacheKey) {
  const quint64 next = m_cacheKeyIssuedGeneration.value(cacheKey, 0) + 1;
  m_cacheKeyIssuedGeneration.insert(cacheKey, next);
  return next;
}

bool AssetRequestCoordinator::tryApplyCacheKeyMutation(
    const QString &cacheKey, quint64 issuedGeneration) {
  if (issuedGeneration < currentCacheKeyGeneration(cacheKey)) {
    // A strictly newer issuance has already been applied to this cache
    // key -- see the declaration comment for the full ordering argument.
    return false;
  }
  m_cacheKeyGeneration.insert(cacheKey, issuedGeneration);
  return true;
}

QSet<QString> AssetRequestCoordinator::activeInFlightCacheKeys() const {
  QSet<QString> active;
  for (auto it = m_operations.constBegin(); it != m_operations.constEnd();
       ++it) {
    const Operation &operation = it.value();
    if (operation.isRevalidation) {
      if (!operation.revalidationCacheKey.isEmpty()) {
        active.insert(operation.revalidationCacheKey);
      }
      continue;
    }
    if (operation.candidateIndex >= 0 &&
        operation.candidateIndex < operation.candidates.size()) {
      active.insert(AssetCache::cacheKeyFor(
          operation.candidates[operation.candidateIndex].url));
    }
  }
  return active;
}

void AssetRequestCoordinator::pruneStaleCacheKeyState() {
  const QSet<QString> active = activeInFlightCacheKeys();
  const qint64 now = m_monotonicNowMs();

  // See the declaration comment: a negative-404 record is prunable once
  // it is no longer authoritative (expired, or superseded by a
  // different generation having since been applied) -- the same
  // condition hasNegative404() already checks lazily -- AND its cache
  // key is not pinned by any still-in-flight operation.
  for (auto it = m_negative404.begin(); it != m_negative404.end();) {
    const bool stillAuthoritative =
        it->generation == m_cacheKeyGeneration.value(it.key(), 0) &&
        now < it->expiresAtMonotonicMs;
    if (!stillAuthoritative && !active.contains(it.key())) {
      it = m_negative404.erase(it);
    } else {
      ++it;
    }
  }

  // Review round-4 item 7: enforce the hard ceiling even when every
  // surviving record is individually still within its TTL -- evict the
  // soonest-to-expire non-pinned record(s) first (a reasonable,
  // deterministic proxy for "oldest" given these records carry no
  // separate last-access timestamp) until back at or under the cap, or
  // until every remaining record is pinned by an in-flight operation
  // (in which case the cap is temporarily, unavoidably exceeded by
  // however many operations are genuinely still in flight).
  while (m_negative404.size() > m_maxTrackedNegative404Entries) {
    auto oldestIt = m_negative404.end();
    for (auto it = m_negative404.begin(); it != m_negative404.end(); ++it) {
      if (active.contains(it.key())) {
        continue;
      }
      if (oldestIt == m_negative404.end() ||
          it->expiresAtMonotonicMs < oldestIt->expiresAtMonotonicMs) {
        oldestIt = it;
      }
    }
    if (oldestIt == m_negative404.end()) {
      break;
    }
    m_negative404.erase(oldestIt);
  }

  // Generation-tracking state for a cache key is prunable once nothing
  // still needs to compare against it: no in-flight operation is pinning
  // it, and no surviving (just-swept, still-authoritative) negative-404
  // record depends on it either.
  for (auto it = m_cacheKeyGeneration.begin();
       it != m_cacheKeyGeneration.end();) {
    if (!active.contains(it.key()) && !m_negative404.contains(it.key())) {
      it = m_cacheKeyGeneration.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = m_cacheKeyIssuedGeneration.begin();
       it != m_cacheKeyIssuedGeneration.end();) {
    if (!active.contains(it.key()) && !m_negative404.contains(it.key())) {
      it = m_cacheKeyIssuedGeneration.erase(it);
    } else {
      ++it;
    }
  }
}

AssetOutcome<AssetCache::CachedEntry>
AssetRequestCoordinator::ensureDecoded(AssetCache::CachedEntry entry,
                                       AssetFormat format) {
  if (!entry.decodedImage.isNull()) {
    return AssetOutcome<AssetCache::CachedEntry>(std::move(entry));
  }

  ++m_realDecodeCallCountForTesting;
  const AssetOutcome<QImage> decoded =
      m_fetcher.decodeAndValidate(entry.encodedBytes, format);
  if (!decoded) {
    return AssetOutcome<AssetCache::CachedEntry>(decoded.error());
  }

  // Round-4 review item 9: a disk hit's metadata (contentType/dimensions)
  // is read back from a JSON file this process itself does not
  // continuously guard -- it can be corrupted independently of the
  // payload it describes (disk bitrot touching only the metadata file,
  // a partially-applied external edit, or an adversary who can write
  // into the cache directory but does not control this process). The
  // payload's own byte-for-byte integrity (sha256Hex/encodedSize) is
  // already fully re-verified by AssetCache::lookupDisk() before this
  // function ever runs -- but that only proves the ENCODED bytes are
  // exactly what was originally stored, not that the METADATA describing
  // them (which lookupDisk() reads from a separate file and trusts
  // verbatim into CachedEntry::contentType/dimensions) still agrees with
  // what those bytes actually, truly decode to. Cross-validate both
  // fields against the just-decoded, ground-truth QImage/candidate
  // format now: a mismatch means the metadata is self-inconsistent with
  // its own payload (zero/wrong-but-well-formed JSON, or a stale field
  // left over from some earlier, different generation) and must never be
  // served -- CacheCorrupt routes this exact case through the same
  // quarantine-then-single-fresh-refetch path
  // completeCacheReadOrQuarantine() already uses for a hard decode
  // failure (see isQuarantineWorthy()).
  //
  // entry.contentType is compared against assetFormatMimeType(format)
  // rather than re-deriving it from magic bytes a second time here:
  // decodeAndValidate() above already independently confirmed (via
  // sniffMagicBytes()) that the ENCODED bytes' magic bytes match
  // `format` exactly, and every legitimate store() call persists
  // contentType as precisely assetFormatMimeType() of the format the
  // bytes were fetched/validated against (see
  // AssetNetworkFetcher::fetch()'s declaredContentType check) -- so this
  // comparison catches a metadata file whose contentType field was
  // corrupted/tampered/stale independently of the payload, without
  // redundantly re-implementing the magic-byte check.
  if (entry.contentType != assetFormatMimeType(format) ||
      entry.dimensions != decoded->size()) {
    return AssetOutcome<AssetCache::CachedEntry>(AssetError{
        AssetErrorCode::CacheCorrupt,
        QStringLiteral("cached metadata (contentType/dimensions) does not "
                       "match the actual decoded image")});
  }

  entry.decodedImage = *decoded;
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
    AssetCache::CachedEntry entry, QString cacheKey,
    quint64 expectedGeneration) {
  const quint64 handleId = m_nextHandle++;
  const quint64 operationId = m_nextOperationId++;

  // Same "candidate's own format, not the logical key's" rule
  // completeCacheReadOrQuarantine() applies -- computed here, before
  // `candidates` is moved into the Operation below, so it can also key
  // this registration's shared-decode group (see PendingCacheDecode's
  // comment).
  const AssetFormat expectedFormat =
      candidateIndex >= 0 && candidateIndex < candidates.size()
          ? candidates[candidateIndex].format
          : key.format;

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

  // Round-7/8 item 7 ("cache-hit read/decode occurs before operation
  // coalescing"): join (or become the leader of) a shared decode for this
  // exact (cacheKey, format) pair instead of always scheduling an
  // independent one -- see PendingCacheDecode's declaration comment.
  QString decodeKey = cacheDecodeCoalescingKey(cacheKey, expectedFormat);
  auto pendingIt = m_pendingCacheDecodes.find(decodeKey);
  if (pendingIt != m_pendingCacheDecodes.end()) {
    if (pendingIt->entry.encodedBytes == entry.encodedBytes) {
      // A leader is already registered for the identical cached bytes:
      // attach as an additional waiter. No second queued decode is
      // scheduled -- the leader's own already-queued
      // completeCoalescedCacheDecode() call delivers to every waiter,
      // including this one, once it runs.
      pendingIt->waiters.append(GroupWaiter{operationId, expectedGeneration});
      return RequestHandle{handleId};
    }
    // Defensive only: the cache's live bytes for this exact key changed
    // between two near-simultaneous lookups (e.g. an unrelated fetch
    // completed and republished the key in between this call and the
    // still-pending leader's queued decode). Never silently merge two
    // different byte sequences into one waiter list, and never overwrite
    // the existing map slot either -- doing so would orphan the existing
    // leader's already-queued closure and strand its waiters. Disambiguate
    // this call's own key instead, so it becomes an independent leader
    // that can never collide with the existing group; this is exactly as
    // safe as (merely not any MORE coalesced than) the pre-coalescing
    // behaviour of always scheduling an independent decode.
    decodeKey += QLatin1Char('#') + QString::number(operationId);
  }

  m_pendingCacheDecodes.insert(
      decodeKey,
      PendingCacheDecode{std::move(entry),
                         cacheKey,
                         expectedFormat,
                         {GroupWaiter{operationId, expectedGeneration}}});

  QPointer<AssetRequestCoordinator> self(this);
  QMetaObject::invokeMethod(
      this,
      [self, decodeKey]() {
        if (self) {
          self->completeCoalescedCacheDecode(decodeKey);
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
    quint64 expectedGeneration, bool promoteOnSuccess) {
  auto it = m_operations.find(operationId);
  if (it == m_operations.end()) {
    return; // cancelled/destroyed before this queued call ran
  }
  const Operation &operation = it.value();
  // Item 1 follow-on: a cached entry's expected format is the CANDIDATE's
  // format (see AssetCandidate::format), not the logical key's -- a
  // CardBackKind::GenericEncounterBack/GenericPlayerBack/CustomBack
  // candidate is fetched/decoded/quarantined against its own format,
  // which can differ from AssetLocator::canonicalFormatFor(key.category).
  const AssetFormat expectedFormat =
      operation.candidateIndex < operation.candidates.size()
          ? operation.candidates[operation.candidateIndex].format
          : operation.key.format;
  // A single-waiter group: see completeCacheReadGroupOrQuarantine()'s
  // comment. Every one of the branches that method implements (success/
  // CAS/promote, non-quarantine-worthy failure, quarantine-worthy
  // invalidate-once-then-retry) is bit-for-bit identical to what this
  // method used to implement directly for exactly one operationId.
  completeCacheReadGroupOrQuarantine(
      {GroupWaiter{operationId, expectedGeneration}}, std::move(entry),
      cacheKey, expectedFormat, promoteOnSuccess);
}

QString
AssetRequestCoordinator::cacheDecodeCoalescingKey(const QString &cacheKey,
                                                  AssetFormat format) {
  return cacheKey + QLatin1Char('|') +
         QString::number(static_cast<int>(format));
}

void AssetRequestCoordinator::completeCacheReadGroupOrQuarantine(
    const QVector<GroupWaiter> &waiters, AssetCache::CachedEntry entry,
    const QString &cacheKey, AssetFormat expectedFormat,
    bool promoteOnSuccess) {
  AssetOutcome<AssetCache::CachedEntry> outcome =
      ensureDecoded(std::move(entry), expectedFormat);

  if (outcome) {
    // Review round-3 items 14/15: gate BOTH the memory-decoded-image
    // republish (ensureDecoded() is deliberately side-effect-free -- see
    // its comment) and, when requested, the full promoteToMemory() behind
    // the SAME issuance-ordered CAS (tryApplyCacheKeyMutation()), applied
    // independently per waiter and in registration order -- so the FINAL
    // applied generation for `cacheKey` is exactly what fully independent,
    // one-decode-per-operation processing would have left behind, even
    // though the decode itself ran only once for the whole group.
    for (const GroupWaiter &waiter : waiters) {
      if (m_operations.find(waiter.operationId) == m_operations.end()) {
        continue; // cancelled before delivery -- see header comment
      }
      AssetOutcome<AssetCache::CachedEntry> perWaiterOutcome(*outcome);
      if (tryApplyCacheKeyMutation(cacheKey, waiter.expectedGeneration)) {
        m_cache.updateMemoryDecodedImage(cacheKey,
                                         perWaiterOutcome->decodedImage);
        if (promoteOnSuccess) {
          m_cache.promoteToMemory(cacheKey, *perWaiterOutcome);
        }
      }
      completeOperation(waiter.operationId, std::move(perWaiterOutcome));
    }
    return;
  }

  if (!isQuarantineWorthy(outcome.error().code)) {
    for (const GroupWaiter &waiter : waiters) {
      if (m_operations.find(waiter.operationId) == m_operations.end()) {
        continue;
      }
      completeOperation(waiter.operationId,
                        AssetOutcome<AssetCache::CachedEntry>(outcome.error()));
    }
    return;
  }

  // Review item 9 (refined by round-7/8 item 6, "cache-hit read/decode
  // occurs before operation coalescing"): this exact cached generation
  // just failed a format/magic/decode/dimension/pixel-budget re-check
  // against CURRENT limits -- it must never be served again as-is, and
  // never silently keep failing every future request for the same key
  // forever. Evict both the memory and disk state for this exact
  // resolved-candidate cache key EXACTLY ONCE for the whole group (never
  // once per waiter -- see the header comment), then let every waiter
  // whose own CAS still applies independently retry precisely the SAME
  // candidate as a genuine network miss; a waiter whose CAS no longer
  // applies (superseded by a newer issuance since this entry was read)
  // simply reports the failure this stale view genuinely observed,
  // exactly as the single-operation path always did.
  bool invalidatedThisGroup = false;
  for (const GroupWaiter &waiter : waiters) {
    auto it = m_operations.find(waiter.operationId);
    if (it == m_operations.end()) {
      continue;
    }
    if (!tryApplyCacheKeyMutation(cacheKey, waiter.expectedGeneration)) {
      completeOperation(waiter.operationId,
                        AssetOutcome<AssetCache::CachedEntry>(outcome.error()));
      continue;
    }
    if (!invalidatedThisGroup) {
      (void)m_cache.invalidate(cacheKey);
      invalidatedThisGroup = true;
    }
    Operation &operation = it.value();
    operation.isRevalidation = false;
    operation.staleEntry.reset();
    startCandidate(waiter.operationId);
  }
}

void AssetRequestCoordinator::completeCoalescedCacheDecode(
    const QString &decodeKey) {
  auto it = m_pendingCacheDecodes.find(decodeKey);
  if (it == m_pendingCacheDecodes.end()) {
    return; // see the header comment
  }
  PendingCacheDecode pending = std::move(it.value());
  m_pendingCacheDecodes.erase(it);
  completeCacheReadGroupOrQuarantine(pending.waiters, std::move(pending.entry),
                                     pending.cacheKey, pending.format,
                                     /*promoteOnSuccess=*/false);
}

void AssetRequestCoordinator::pruneCancelledPendingCacheDecodeWaiter(
    quint64 operationId) {
  for (auto it = m_pendingCacheDecodes.begin();
       it != m_pendingCacheDecodes.end();) {
    QVector<GroupWaiter> &waiters = it->waiters;
    for (int i = waiters.size() - 1; i >= 0; --i) {
      if (waiters[i].operationId == operationId) {
        waiters.remove(i);
      }
    }
    if (waiters.isEmpty()) {
      // Every waiter that ever joined this group has now cancelled
      // before its queued completeCoalescedCacheDecode() closure ran --
      // erase the group entirely so that closure's own missing-entry
      // check makes it a genuine no-op: the shared decode this group
      // was formed for never happens at all.
      it = m_pendingCacheDecodes.erase(it);
    } else {
      ++it;
    }
  }
}

void AssetRequestCoordinator::advanceCandidates(quint64 operationId) {
  auto it = m_operations.find(operationId);
  if (it == m_operations.end()) {
    return; // cancelled/destroyed
  }
  Operation &operation = it.value();
  operation.isRevalidation = false;
  operation.staleEntry.reset();

  // Review round-3 item 12: identical priority-order/cache-then-network
  // scan as request()'s own initial loop (see its comment) -- the ONE
  // path every transition (the very first untried candidate, and every
  // later 404/quarantine-driven advance) uses, so a localized-then-404
  // candidate can never bypass an already-cached lower-priority candidate
  // (e.g. an English fallback) by jumping straight to a network fetch for
  // the next index without first re-checking negative-404/memory/disk for
  // it.
  for (; operation.candidateIndex < operation.candidates.size();
       ++operation.candidateIndex) {
    const AssetCandidate &candidate =
        operation.candidates[operation.candidateIndex];
    const QString cacheKey = AssetCache::cacheKeyFor(candidate.url);

    if (hasNegative404(cacheKey)) {
      continue; // authoritatively confirmed absent: try the next candidate
    }

    if (auto hit = m_cache.lookupMemory(cacheKey)) {
      completeCacheReadOrQuarantine(operationId, std::move(*hit), cacheKey,
                                    issueCacheKeyGeneration(cacheKey),
                                    /*promoteOnSuccess=*/false);
      return;
    }

    if (auto hit = m_cache.lookupDisk(cacheKey)) {
      AssetCache::CachedEntry entry = *hit;
      if (entry.etag.isEmpty() && entry.lastModified.isEmpty()) {
        completeCacheReadOrQuarantine(operationId, std::move(entry), cacheKey,
                                      issueCacheKeyGeneration(cacheKey),
                                      /*promoteOnSuccess=*/false);
        return;
      }
      operation.isRevalidation = true;
      operation.revalidationCacheKey = cacheKey;
      operation.staleEntry = std::move(entry);
      startRevalidation(operationId);
      return;
    }

    // Neither a confirmed-absent record nor a cache hit: the first
    // genuinely untried candidate from here -- fall through to the
    // network.
    startCandidate(operationId);
    return;
  }

  // Every remaining candidate carries an authoritative confirmed-404
  // record (or none remain at all): the whole logical request is
  // definitively not-found, with no further network round trip.
  completeOperation(
      operationId,
      AssetOutcome<AssetCache::CachedEntry>(AssetError{
          AssetErrorCode::NotFound,
          QStringLiteral("every candidate previously confirmed absent "
                         "(negative cache)")}));
}

void AssetRequestCoordinator::startCandidate(quint64 operationId) {
  auto it = m_operations.find(operationId);
  if (it == m_operations.end()) {
    return;
  }
  Operation &operation = it.value();
  const AssetCandidate &candidate =
      operation.candidates[operation.candidateIndex];
  const QString cacheKey = AssetCache::cacheKeyFor(candidate.url);

  // Review round-4 item 1: the network transport's fetch() no longer
  // accepts a bare QUrl at all (see AssetFetchUrl's class comment in
  // AssetNetworkFetcher.h) -- every candidate URL, even one produced by
  // this project's own trusted AssetLocator, must still pass through the
  // same validating factory every other caller does. A real candidate
  // always satisfies it trivially; this can only fail if AssetLocator's
  // own resolution logic ever regresses, in which case failing this one
  // operation closed with a typed error is correct and safe -- never a
  // silent fall-through to an unvalidated fetch.
  const AssetOutcome<AssetFetchUrl> validatedUrl =
      AssetFetchUrl::validate(candidate.url);
  if (!validatedUrl) {
    completeOperation(operationId, AssetOutcome<AssetCache::CachedEntry>(
                                       validatedUrl.error()));
    return;
  }

  // Round-6 item 8: an unconditional fetch is identified purely by
  // cacheKey+format (empty etag/lastModified) -- join an already in-
  // flight identical attempt (necessarily belonging to a DIFFERENT
  // logical AssetKey; findInFlightOperation() already caught the
  // identical-AssetKey case before this operation was ever created) if
  // one exists, instead of issuing a second redundant HTTP request.
  const QString attemptKey =
      candidateAttemptKey(cacheKey, candidate.format, QString(), QString());
  auto attemptIt = m_candidateAttempts.find(attemptKey);
  if (attemptIt != m_candidateAttempts.end()) {
    attemptIt->subscriberOperationIds.append(operationId);
    operation.pendingAttemptKey = attemptKey;
    operation.fetchHandle = attemptIt->fetchHandle;
    return;
  }

  // Review round-3 item 14: the ISSUANCE value minted for this exact
  // candidate's cache key AT THE MOMENT this fetch is issued -- the CAS
  // baseline a completion callback (however late it arrives, and
  // regardless of completion order relative to any other operation on
  // this same cache key) is checked against before it is allowed to
  // mutate shared cache/negative-404 state. See
  // tryApplyCacheKeyMutation()'s comment and the class comment's
  // "Cross-logical-key races" paragraph. Shared by every subscriber that
  // later joins this same attempt -- see CandidateAttempt's comment.
  const quint64 expectedGeneration = issueCacheKeyGeneration(cacheKey);

  CandidateAttempt attempt;
  attempt.cacheKey = cacheKey;
  attempt.issuedGeneration = expectedGeneration;
  attempt.subscriberOperationIds.append(operationId);
  // Round-9+ review (HIGH): mint this attempt's own immutable identity
  // BEFORE inserting it, and capture it (not just attemptKey) into the
  // completion lambda below -- see CandidateAttempt::token's comment.
  const quint64 attemptToken = m_nextCandidateAttemptToken++;
  attempt.token = attemptToken;
  m_candidateAttempts.insert(attemptKey, attempt);
  operation.pendingAttemptKey = attemptKey;

  QPointer<AssetRequestCoordinator> self(this);
  const AssetFormat format = candidate.format;
  const AssetNetworkFetcher::FetchHandle fetchHandle = m_fetcher.fetch(
      *validatedUrl, format, {},
      [self, attemptKey, attemptToken, cacheKey, expectedGeneration](
          AssetOutcome<AssetNetworkFetcher::ConditionalFetchResult> result) {
        if (!self) {
          return;
        }
        // Round-6 item 8: extract the full subscriber list and remove
        // this attempt from the map FIRST -- before applying any
        // mutation or dispatching to a single subscriber -- so a
        // request for the identical candidate arriving from WITHIN this
        // very callback (e.g. one subscriber's own definitive-404
        // advance immediately re-resolving to a candidate another
        // subscriber is also about to need) always starts a genuinely
        // fresh attempt rather than appending to one that has already
        // fired.
        //
        // Round-9+ review (HIGH): `attemptKey` alone is NOT sufficient
        // to identify "this exact attempt" -- see
        // CandidateAttempt::token's comment for the full race this
        // closes. A missing entry, OR an entry present but bearing a
        // DIFFERENT token than the one this callback captured at
        // creation time, both mean "not mine any more": some other,
        // newer attempt now occupies (or once occupied and has already
        // been erased from) this same string key, and this callback
        // must never touch it.
        auto attemptIt2 = self->m_candidateAttempts.find(attemptKey);
        if (attemptIt2 == self->m_candidateAttempts.end() ||
            attemptIt2->token != attemptToken) {
          return;
        }
        const QVector<quint64> subscribers = attemptIt2->subscriberOperationIds;
        self->m_candidateAttempts.erase(attemptIt2);
        self->dispatchCandidateFetchResult(cacheKey, expectedGeneration,
                                           subscribers, std::move(result));
      });
  // Safe to set AFTER the fetch() call returns: fetch()'s callback
  // contract is always dispatched asynchronously (Qt::QueuedConnection or
  // an equivalent queued invocation), never synchronously from within
  // fetch() itself -- see AssetNetworkFetcher::fetch()'s own comments --
  // so the completion lambda above can never run before this line, and
  // every subscriber (this one and any later joiner) observes a fully
  // populated attempt.
  m_candidateAttempts[attemptKey].fetchHandle = fetchHandle;
  operation.fetchHandle = fetchHandle;
}

void AssetRequestCoordinator::dispatchCandidateFetchResult(
    const QString &cacheKey, quint64 issuedGeneration,
    const QVector<quint64> &subscribers,
    AssetOutcome<AssetNetworkFetcher::ConditionalFetchResult> result) {
  // Round-6 item 8: apply the CAS-guarded cache/negative-404 mutation for
  // `cacheKey` AT MOST ONCE for the whole coalesced group -- see the
  // class comment's "Cross-logical-key races" paragraph and
  // CandidateAttempt's declaration comment. Every subscriber below is
  // handed the SAME verdict; per-subscriber dispatch never re-derives or
  // re-applies the mutation.
  bool isDefinitiveNotFound = false;
  bool notFoundPersistenceFailed = false;
  bool isSuccess = false;
  std::optional<AssetCache::CachedEntry> successEntry;

  if (!result) {
    if (result.error().code == AssetErrorCode::NotFound) {
      isDefinitiveNotFound = true;
      // See startCandidate()'s previous (pre-round-6-item-8) comment for
      // the full rationale: a definitive 404 both tombstones any
      // existing entry and records a negative-404, gated by the CAS
      // check so a stale 404 can never resurrect a negative record over
      // a cache key a more recently issued operation has already
      // populated with a genuinely fresh success.
      if (tryApplyCacheKeyMutation(cacheKey, issuedGeneration)) {
        const AssetCache::InvalidateResult invalidateResult =
            m_cache.invalidate(cacheKey);
        if (invalidateResult ==
            AssetCache::InvalidateResult::PersistenceFailed) {
          notFoundPersistenceFailed = true;
        } else {
          recordNegative404(cacheKey, issuedGeneration);
        }
      }
    }
  } else if (result->notModified || !result->asset.has_value()) {
    // fetch() is always called here without conditional headers, so this
    // is unreachable in practice -- handled defensively only, exactly as
    // before.
  } else {
    isSuccess = true;
    AssetCache::CachedEntry entry = toCachedEntry(*result->asset);
    if (tryApplyCacheKeyMutation(cacheKey, issuedGeneration)) {
      clearNegative404(cacheKey);
      m_cache.store(cacheKey, entry);
    }
    successEntry = entry;
  }

  for (const quint64 subscriberId : subscribers) {
    auto opIt = m_operations.find(subscriberId);
    if (opIt == m_operations.end()) {
      continue; // this subscriber was independently cancelled already
    }
    Operation &operation = opIt.value();
    operation.pendingAttemptKey.clear();

    if (!result) {
      if (isDefinitiveNotFound) {
        if (notFoundPersistenceFailed) {
          completeOperation(
              subscriberId,
              AssetOutcome<AssetCache::CachedEntry>(AssetError{
                  AssetErrorCode::CachePersistenceFailed,
                  QStringLiteral("failed to durably invalidate a "
                                 "definitively 404'd cache entry")}));
          continue;
        }
        if (operation.candidateIndex + 1 < operation.candidates.size()) {
          // Review round-3 item 12: route this advance through the
          // single shared candidate-processing path.
          ++operation.candidateIndex;
          advanceCandidates(subscriberId);
          continue;
        }
      }
      completeOperation(subscriberId,
                        AssetOutcome<AssetCache::CachedEntry>(result.error()));
      continue;
    }

    if (!isSuccess) {
      completeOperation(
          subscriberId,
          AssetOutcome<AssetCache::CachedEntry>(
              AssetError{AssetErrorCode::ConditionalWithoutCachedBody,
                         QStringLiteral("unexpected not-modified result for an "
                                        "unconditional fetch")}));
      continue;
    }

    completeOperation(subscriberId,
                      AssetOutcome<AssetCache::CachedEntry>(*successEntry));
  }
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
  const QString cacheKey = operation.revalidationCacheKey;

  AssetNetworkFetcher::ConditionalHeaders conditional;
  conditional.etag = staleEntry.etag;
  conditional.lastModified = staleEntry.lastModified;

  // Review round-4 item 1: see startCandidate()'s identical comment.
  const AssetOutcome<AssetFetchUrl> validatedUrl =
      AssetFetchUrl::validate(candidate.url);
  if (!validatedUrl) {
    completeOperation(operationId, AssetOutcome<AssetCache::CachedEntry>(
                                       validatedUrl.error()));
    return;
  }

  // Round-6 item 8: identical role to startCandidate()'s attempt-join --
  // see candidateAttemptKey()'s comment. The conditional validator
  // snapshot is part of the key so a revalidation carrying different
  // ETag/Last-Modified values (which should not normally happen for the
  // same cacheKey, since the disk cache backing both is shared, but is
  // not structurally impossible if two operations raced a stale-read at
  // different moments) is never wrongly coalesced with one carrying a
  // stale validator.
  const QString attemptKey = candidateAttemptKey(
      cacheKey, candidate.format, staleEntry.etag, staleEntry.lastModified);
  auto attemptIt = m_candidateAttempts.find(attemptKey);
  if (attemptIt != m_candidateAttempts.end()) {
    attemptIt->subscriberOperationIds.append(operationId);
    operation.pendingAttemptKey = attemptKey;
    operation.fetchHandle = attemptIt->fetchHandle;
    return;
  }

  // Review round-3 item 14: the ISSUANCE value minted for this exact
  // revalidation's cache key AT THE MOMENT it is issued -- see
  // startCandidate()'s identical comment, tryApplyCacheKeyMutation()'s
  // comment, and the class comment's "Cross-logical-key races" paragraph.
  // Shared by every subscriber that later joins this same attempt.
  const quint64 expectedGeneration = issueCacheKeyGeneration(cacheKey);

  CandidateAttempt attempt;
  attempt.cacheKey = cacheKey;
  attempt.issuedGeneration = expectedGeneration;
  attempt.isRevalidation = true;
  attempt.subscriberOperationIds.append(operationId);
  // Round-9+ review (HIGH): see startCandidate()'s identical comment
  // and CandidateAttempt::token's declaration comment.
  const quint64 attemptToken = m_nextCandidateAttemptToken++;
  attempt.token = attemptToken;
  m_candidateAttempts.insert(attemptKey, attempt);
  operation.pendingAttemptKey = attemptKey;

  QPointer<AssetRequestCoordinator> self(this);
  const AssetFormat format = candidate.format;
  const AssetNetworkFetcher::FetchHandle fetchHandle = m_fetcher.fetch(
      *validatedUrl, format, conditional,
      [self, attemptKey, attemptToken, cacheKey, expectedGeneration](
          AssetOutcome<AssetNetworkFetcher::ConditionalFetchResult> result) {
        if (!self) {
          return;
        }
        // Round-6 item 8: see startCandidate()'s identical comment --
        // extract subscribers and remove the attempt from the map
        // before any dispatch. Round-9+ review (HIGH): token-gated
        // exactly like startCandidate()'s completion lambda -- see
        // CandidateAttempt::token's comment.
        auto attemptIt2 = self->m_candidateAttempts.find(attemptKey);
        if (attemptIt2 == self->m_candidateAttempts.end() ||
            attemptIt2->token != attemptToken) {
          return;
        }
        const QVector<quint64> subscribers = attemptIt2->subscriberOperationIds;
        self->m_candidateAttempts.erase(attemptIt2);
        self->dispatchRevalidationResult(cacheKey, expectedGeneration,
                                         subscribers, std::move(result));
      });
  // Safe to set AFTER fetch() returns -- see startCandidate()'s identical
  // comment.
  m_candidateAttempts[attemptKey].fetchHandle = fetchHandle;
  operation.fetchHandle = fetchHandle;
}

void AssetRequestCoordinator::dispatchRevalidationResult(
    const QString &cacheKey, quint64 issuedGeneration,
    const QVector<quint64> &subscribers,
    AssetOutcome<AssetNetworkFetcher::ConditionalFetchResult> result) {
  // Round-6 item 8: apply the CAS-guarded mutation for `cacheKey` AT MOST
  // ONCE for the whole coalesced group -- see startCandidate()'s
  // dispatchCandidateFetchResult() for the identical principle. Each
  // subscriber below is still independently routed through
  // completeCacheReadOrQuarantine()/completeOperation() using THAT
  // subscriber's own staleEntry/candidateIndex, even though the network
  // round trip and cache mutation were shared.
  enum class Verdict {
    NotFoundFailedClosed,
    NotFoundAdvance,
    StaleIfError,
    NotModifiedPromote,
    FreshReplace,
  };
  Verdict verdict;
  AssetCache::CachedEntry freshEntry;

  if (!result && result.error().code == AssetErrorCode::NotFound) {
    // A definitive 404 is the ONE revalidation failure that does NOT
    // fall back to "stale-if-error" (review item 5) -- see
    // startRevalidation()'s previous (pre-round-6-item-8) comment for
    // the full rationale.
    bool persistenceFailed = false;
    if (tryApplyCacheKeyMutation(cacheKey, issuedGeneration)) {
      const AssetCache::InvalidateResult invalidateResult =
          m_cache.invalidate(cacheKey);
      if (invalidateResult == AssetCache::InvalidateResult::PersistenceFailed) {
        persistenceFailed = true;
      } else {
        recordNegative404(cacheKey, issuedGeneration);
      }
    }
    verdict = persistenceFailed ? Verdict::NotFoundFailedClosed
                                : Verdict::NotFoundAdvance;
  } else if (!result) {
    // "Stale-if-error": every OTHER revalidation failure (transport
    // error, timeout, TLS failure, 5xx, an integrity/codec failure,
    // cancellation racing a teardown, etc.) attempts to serve the still-
    // valid stale cached entry -- see startRevalidation()'s previous
    // comment for the full rationale.
    verdict = Verdict::StaleIfError;
  } else if (result->notModified) {
    // Confirmed unchanged: refresh lastAccess, and adopt any validator
    // the 304 itself refreshed -- see startRevalidation()'s previous
    // comment for the full rationale, including why no separate "post-
    // touch generation" is needed.
    if (tryApplyCacheKeyMutation(cacheKey, issuedGeneration)) {
      m_cache.touchAfterNotModified(cacheKey, result->refreshedEtag,
                                    result->refreshedLastModified);
    }
    verdict = Verdict::NotModifiedPromote;
  } else if (!result->asset.has_value()) {
    // Defensive only: AssetNetworkFetcher never returns notModified==
    // false with an empty asset.
    verdict = Verdict::StaleIfError;
  } else {
    // The origin sent a fresh 200 body despite our conditional headers
    // (its content genuinely changed): replace the cached entry.
    freshEntry = toCachedEntry(*result->asset);
    if (tryApplyCacheKeyMutation(cacheKey, issuedGeneration)) {
      m_cache.store(cacheKey, freshEntry);
    }
    verdict = Verdict::FreshReplace;
  }

  // Round-9+ review item 3/7: accumulators for the batched, single-decode
  // StaleIfError/NotModifiedPromote groups built up by the loop below --
  // see the loop's own comments on each verdict case.
  QVector<GroupWaiter> staleIfErrorGroupWaiters;
  std::optional<AssetCache::CachedEntry> staleIfErrorGroupEntry;
  AssetFormat staleIfErrorGroupFormat = AssetFormat::Png;
  QVector<GroupWaiter> notModifiedGroupWaiters;
  std::optional<AssetCache::CachedEntry> notModifiedGroupEntry;
  AssetFormat notModifiedGroupFormat = AssetFormat::Png;

  for (const quint64 subscriberId : subscribers) {
    auto opIt = m_operations.find(subscriberId);
    if (opIt == m_operations.end()) {
      continue; // this subscriber was independently cancelled already
    }
    Operation &operation = opIt.value();
    operation.pendingAttemptKey.clear();
    const AssetCache::CachedEntry stale = *operation.staleEntry;

    switch (verdict) {
    case Verdict::NotFoundFailedClosed:
      completeOperation(subscriberId,
                        AssetOutcome<AssetCache::CachedEntry>(AssetError{
                            AssetErrorCode::CachePersistenceFailed,
                            QStringLiteral("failed to durably invalidate a "
                                           "definitively 404'd cache entry")}));
      break;
    case Verdict::NotFoundAdvance:
      if (operation.candidateIndex + 1 < operation.candidates.size()) {
        // Review round-3 item 12: route this advance through the single
        // shared candidate-processing path -- see advanceCandidates()'s
        // comment -- which itself resets isRevalidation/staleEntry
        // before scanning forward.
        ++operation.candidateIndex;
        advanceCandidates(subscriberId);
      } else {
        completeOperation(subscriberId, AssetOutcome<AssetCache::CachedEntry>(
                                            result.error()));
      }
      break;
    case Verdict::StaleIfError:
      // Round-9+ review item 3/7 ("aliases coalesce network but not
      // cached/304 decode"): every subscriber of THIS attempt shares the
      // identical (cacheKey, format, etag, lastModified) attempt key --
      // see candidateAttemptKey()'s comment -- so their staleEntry is
      // expected to be byte-identical too. Defer to the batched,
      // single-decode path below instead of decoding independently per
      // subscriber; the (rare, defensive-only) case where a subscriber's
      // captured stale entry unexpectedly disagrees still falls back to
      // an independent decode, exactly as registerCacheHitCompletion()
      // does for the analogous cache-hit case.
      if (!staleIfErrorGroupEntry.has_value()) {
        staleIfErrorGroupEntry = stale;
        staleIfErrorGroupFormat =
            operation.candidateIndex >= 0 &&
                    operation.candidateIndex < operation.candidates.size()
                ? operation.candidates[operation.candidateIndex].format
                : operation.key.format;
        staleIfErrorGroupWaiters.append(
            GroupWaiter{subscriberId, issuedGeneration});
      } else if (staleIfErrorGroupEntry->encodedBytes == stale.encodedBytes) {
        staleIfErrorGroupWaiters.append(
            GroupWaiter{subscriberId, issuedGeneration});
      } else {
        completeCacheReadOrQuarantine(subscriberId, stale, cacheKey,
                                      issuedGeneration,
                                      /*promoteOnSuccess=*/false);
      }
      break;
    case Verdict::NotModifiedPromote:
      // Same batching principle as StaleIfError above -- a validator-
      // carrying disk hit is normally withheld from memory promotion
      // until it has actually been revalidated; this 304 IS that
      // revalidation, so the batched group below is asked to
      // unconditionally promote the (now-decoded) entry on success.
      if (!notModifiedGroupEntry.has_value()) {
        notModifiedGroupEntry = stale;
        notModifiedGroupFormat =
            operation.candidateIndex >= 0 &&
                    operation.candidateIndex < operation.candidates.size()
                ? operation.candidates[operation.candidateIndex].format
                : operation.key.format;
        notModifiedGroupWaiters.append(
            GroupWaiter{subscriberId, issuedGeneration});
      } else if (notModifiedGroupEntry->encodedBytes == stale.encodedBytes) {
        notModifiedGroupWaiters.append(
            GroupWaiter{subscriberId, issuedGeneration});
      } else {
        completeCacheReadOrQuarantine(subscriberId, stale, cacheKey,
                                      issuedGeneration,
                                      /*promoteOnSuccess=*/true);
      }
      break;
    case Verdict::FreshReplace:
      completeOperation(subscriberId,
                        AssetOutcome<AssetCache::CachedEntry>(freshEntry));
      break;
    }
  }

  // Round-9+ review item 3/7: exactly one decode for the whole
  // StaleIfError group, and exactly one for the whole NotModifiedPromote
  // group -- never one per subscriber. Each waiter still applies its own
  // independent issuance CAS inside completeCacheReadGroupOrQuarantine()
  // (see its own comment), so a subscriber whose logical key has since
  // been superseded still observes exactly the outcome an independent
  // per-operation decode would have produced.
  if (!staleIfErrorGroupWaiters.isEmpty()) {
    completeCacheReadGroupOrQuarantine(
        staleIfErrorGroupWaiters, *staleIfErrorGroupEntry, cacheKey,
        staleIfErrorGroupFormat, /*promoteOnSuccess=*/false);
  }
  if (!notModifiedGroupWaiters.isEmpty()) {
    completeCacheReadGroupOrQuarantine(
        notModifiedGroupWaiters, *notModifiedGroupEntry, cacheKey,
        notModifiedGroupFormat, /*promoteOnSuccess=*/true);
  }
}

void AssetRequestCoordinator::completeOperation(
    quint64 operationId, AssetOutcome<AssetCache::CachedEntry> result) {
  auto it = m_operations.find(operationId);
  if (it == m_operations.end()) {
    return;
  }
  Operation operation = std::move(it.value());
  m_operations.erase(it);
  // Review round-4 item 7: this operation has just stopped being
  // in-flight -- its (or any other now-inactive operation's) cache key(s)
  // may now be prunable from m_negative404/m_cacheKeyGeneration/
  // m_cacheKeyIssuedGeneration. See pruneStaleCacheKeyState()'s
  // declaration comment for the full safety argument; called here,
  // opportunistically, rather than on a timer, so these maps' sizes stay
  // proportional to currently- and recently-active cache keys without
  // any separate scheduling machinery.
  pruneStaleCacheKeyState();
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
    // Last consumer gone: actually abort the underlying fetch -- UNLESS
    // it is shared with one or more other operations via a
    // CandidateAttempt (round-6 item 8), in which case the fetch must
    // keep running for their sake and only this operation's own
    // subscription is removed. Remove the operation from the map FIRST
    // so a reply that was already in flight (racing with this
    // cancellation) can never find and complete it.
    const QString attemptKey = operation.pendingAttemptKey;
    m_operations.erase(opIt);
    const AssetNetworkFetcher::FetchHandle fetchHandleToCancel =
        unsubscribeFromCandidateAttempt(operationId, attemptKey);
    if (fetchHandleToCancel.isValid()) {
      m_fetcher.cancel(fetchHandleToCancel);
    }
    // Round-9+ review item 3/7: this operationId can no longer be
    // delivered to -- see pruneCancelledPendingCacheDecodeWaiter()'s
    // comment for why it must never keep a shared cache-hit decode group
    // (and, transitively, that group's queued near-32-megapixel decode)
    // alive on its own.
    pruneCancelledPendingCacheDecodeWaiter(operationId);
    // Round-6 item 7: this operation has just stopped being in-flight
    // via cancellation, exactly as much as one that stopped via
    // completeOperation() -- its cache key(s) must be equally eligible
    // for pruning from m_negative404/m_cacheKeyGeneration/
    // m_cacheKeyIssuedGeneration (see pruneStaleCacheKeyState()'s
    // comment). Previously only completeOperation() called this, so a
    // burst of uniquely-keyed requests that were each cancelled before
    // ever completing (e.g. a scrolled-past card art) grew
    // m_cacheKeyIssuedGeneration without bound -- activeInFlightCacheKeys()
    // already correctly excludes an erased operation, so this is safe to
    // call immediately after the erase above.
    pruneStaleCacheKeyState();
  }
}

} // namespace Arkham
