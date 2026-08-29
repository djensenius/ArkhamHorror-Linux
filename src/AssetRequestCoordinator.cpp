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
       cacheKey = std::move(cacheKey), expectedGeneration]() mutable {
        if (self) {
          self->completeCacheReadOrQuarantine(operationId, std::move(entry),
                                              cacheKey, expectedGeneration,
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
    quint64 expectedGeneration, bool promoteOnSuccess) {
  auto it = m_operations.find(operationId);
  if (it == m_operations.end()) {
    return; // cancelled/destroyed before this queued call ran
  }
  Operation &operation = it.value();
  // Item 1 follow-on: a cached entry's expected format is the CANDIDATE's
  // format (see AssetCandidate::format), not the logical key's -- a
  // CardBackKind::GenericEncounterBack/GenericPlayerBack/CustomBack
  // candidate is fetched/decoded/quarantined against its own format,
  // which can differ from AssetLocator::canonicalFormatFor(key.category).
  const AssetFormat expectedFormat =
      operation.candidateIndex < operation.candidates.size()
          ? operation.candidates[operation.candidateIndex].format
          : operation.key.format;
  AssetOutcome<AssetCache::CachedEntry> outcome =
      ensureDecoded(std::move(entry), expectedFormat);

  if (outcome) {
    // Review round-3 items 14/15: gate BOTH the memory-decoded-image
    // republish (ensureDecoded() is deliberately side-effect-free -- see
    // its comment) and, when requested, the full promoteToMemory() behind
    // the SAME issuance-ordered CAS (tryApplyCacheKeyMutation()). A
    // decode that raced a newer-issued operation's own publish can
    // therefore never mutate the current live memory entry with stale
    // pixels, regardless of which operation's disk read/decode happens
    // to finish first.
    if (tryApplyCacheKeyMutation(cacheKey, expectedGeneration)) {
      m_cache.updateMemoryDecodedImage(cacheKey, outcome->decodedImage);
      if (promoteOnSuccess) {
        m_cache.promoteToMemory(cacheKey, *outcome);
      }
    }
    completeOperation(operationId, std::move(outcome));
    return;
  }

  if (!isQuarantineWorthy(outcome.error().code)) {
    completeOperation(operationId, std::move(outcome));
    return;
  }

  if (!tryApplyCacheKeyMutation(cacheKey, expectedGeneration)) {
    // Review round-3 item 14: some more recently ISSUED operation has
    // already applied its own mutation for this exact cache key since
    // this entry was read (or since this revalidation was issued) --
    // this now-outdated view failing to decode is NOT proof the CURRENT
    // entry is bad, so never invalidate (which could destroy a newer,
    // perfectly valid entry) and never retry the candidate over the
    // network on this stale path either (which could itself race with,
    // and overwrite, whatever the newer operation just published). Simply
    // report the failure this stale view genuinely observed to this
    // operation's own consumers.
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
  // fail at most once more, never loop. The CAS application above already
  // recorded this exact issuance as the new current generation, so no
  // separate bump is needed here.
  m_cache.invalidate(cacheKey);
  operation.isRevalidation = false;
  operation.staleEntry.reset();
  startCandidate(operationId);
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
  // Review round-3 item 14: the ISSUANCE value minted for this exact
  // candidate's cache key AT THE MOMENT this fetch is issued -- the CAS
  // baseline a completion callback (however late it arrives, and
  // regardless of completion order relative to any other operation on
  // this same cache key) is checked against before it is allowed to
  // mutate shared cache/negative-404 state. See
  // tryApplyCacheKeyMutation()'s comment and the class comment's
  // "Cross-logical-key races" paragraph.
  const quint64 expectedGeneration =
      issueCacheKeyGeneration(AssetCache::cacheKeyFor(candidate.url));

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

  QPointer<AssetRequestCoordinator> self(this);
  operation.fetchHandle = m_fetcher.fetch(
      *validatedUrl, candidate.format, {},
      [self, operationId, expectedGeneration](
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
            const QString cacheKey = AssetCache::cacheKeyFor(
                operation.candidates[operation.candidateIndex].url);
            // Authoritative: this EXACT candidate is definitively absent.
            // Record it so a future request() for a different logical
            // key that also resolves to this same candidate can skip it
            // outright (see hasNegative404() / the class comment) --
            // never recorded for any other error code. Gated by the CAS
            // check (review round-3 item 14): if some more recently
            // ISSUED operation has already applied its own mutation for
            // this cache key (e.g. published a fresh 200) since THIS
            // fetch was issued, this 404 -- observed against an older
            // view of the world -- must not resurrect a negative record
            // over it.
            //
            // Review round-4 item 5: this 404 must ALSO tombstone
            // (invalidate()) any cache entry that may already exist for
            // this exact cache key -- e.g. a genuinely older still-valid
            // 200 published by a DIFFERENT, cross-logical-key operation
            // that happens to resolve to the identical candidate URL
            // (see the class comment's "Cross-logical-key races"
            // paragraph), or a 200 published by an earlier-issued but
            // slower-to-complete operation on this SAME candidate.
            // Previously only startRevalidation()'s 404 path did this;
            // this first-try path recorded a negative-404 record
            // alongside an untouched stale 200 entry, which
            // hasNegative404() correctly hid for as long as the record's
            // TTL lasted -- but once that TTL expired, request()'s
            // ordinary cache lookup would find and serve the
            // never-actually-evicted stale entry again, resurrecting
            // content the origin has since authoritatively confirmed
            // gone. Invalidating here, exactly like the revalidation
            // path already does, closes that gap unconditionally rather
            // than relying on the negative-404 TTL alone.
            if (self->tryApplyCacheKeyMutation(cacheKey, expectedGeneration)) {
              self->m_cache.invalidate(cacheKey);
              self->recordNegative404(cacheKey, expectedGeneration);
            }
            if (operation.candidateIndex + 1 < operation.candidates.size()) {
              // Review round-3 item 12: route this advance through the
              // single shared candidate-processing path -- see
              // advanceCandidates()'s comment -- rather than jumping
              // straight back to a network fetch, so an untried
              // higher-index candidate that happens to already be cached
              // (memory or disk) is never skipped in favour of issuing a
              // redundant network request for it.
              ++operation.candidateIndex;
              self->advanceCandidates(operationId);
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
        // Review round-3 item 14: only publish into the shared
        // cache/negative-404 state if no more recently ISSUED operation
        // has already applied its own mutation for this cache key;
        // otherwise skip the mutation but still hand THIS operation's own
        // consumers the outcome it genuinely fetched (see the class
        // comment).
        if (self->tryApplyCacheKeyMutation(cacheKey, expectedGeneration)) {
          // Defensive: a candidate that once 404'd could, in principle,
          // reappear -- never leave a stale negative record pointing at a
          // now-confirmed-good candidate.
          self->clearNegative404(cacheKey);
          AssetCache::CachedEntry entry = toCachedEntry(*result->asset);
          self->m_cache.store(cacheKey, entry);
          self->completeOperation(operationId,
                                  AssetOutcome<AssetCache::CachedEntry>(entry));
          return;
        }
        AssetCache::CachedEntry entry = toCachedEntry(*result->asset);
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
  // Review round-3 item 14: the ISSUANCE value minted for this exact
  // revalidation's cache key AT THE MOMENT it is issued -- see
  // startCandidate()'s identical comment, tryApplyCacheKeyMutation()'s
  // comment, and the class comment's "Cross-logical-key races" paragraph.
  const quint64 expectedGeneration =
      issueCacheKeyGeneration(operation.revalidationCacheKey);

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

  QPointer<AssetRequestCoordinator> self(this);
  operation.fetchHandle = m_fetcher.fetch(
      *validatedUrl, candidate.format, conditional,
      [self, operationId, expectedGeneration](
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
        const QString &cacheKey = operation.revalidationCacheKey;

        // A definitive 404 is the ONE revalidation failure that does NOT
        // fall back to "stale-if-error" (review item 5): the origin has
        // authoritatively confirmed this exact candidate no longer
        // exists, so the stale cached entry is evicted (never served as
        // a false "still good" success), a negative-404 record is
        // written for it, and the request advances through the
        // remaining candidates exactly like a first-time miss would.
        // Gated by the CAS check (review round-3 item 14): if some more
        // recently ISSUED operation has already applied a fresh entry for
        // this cache key since this revalidation was issued, this 404 --
        // observed against an older view of the world -- must not evict
        // or negative-cache over it.
        if (!result && result.error().code == AssetErrorCode::NotFound) {
          if (self->tryApplyCacheKeyMutation(cacheKey, expectedGeneration)) {
            self->m_cache.invalidate(cacheKey);
            self->recordNegative404(cacheKey, expectedGeneration);
          }
          if (operation.candidateIndex + 1 < operation.candidates.size()) {
            // Review round-3 item 12: route this advance through the
            // single shared candidate-processing path -- see
            // advanceCandidates()'s comment -- which itself resets
            // isRevalidation/staleEntry before scanning forward, rather
            // than jumping straight back to a network fetch and skipping
            // an already-cached higher-index candidate.
            ++operation.candidateIndex;
            self->advanceCandidates(operationId);
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
          self->completeCacheReadOrQuarantine(operationId, stale, cacheKey,
                                              expectedGeneration,
                                              /*promoteOnSuccess=*/false);
          return;
        }

        if (result->notModified) {
          // Confirmed unchanged: refresh lastAccess, and adopt any
          // validator the 304 itself refreshed (RFC 7232 S4.1 allows a
          // 304 to rotate ETag/Last-Modified without a body); the payload
          // bytes are never touched. Empty fields leave the existing
          // validator untouched (see touchAfterNotModified()). Gated by
          // the CAS check (review round-3 item 14): a stale 304 must not
          // touch recency metadata a newer operation has since
          // superseded. Unlike the old completion-ordered scheme, no
          // separate "post-touch generation" is needed here: this touch
          // and completeCacheReadOrQuarantine()'s own subsequent
          // promotion below share the exact same `expectedGeneration`
          // issuance value, and tryApplyCacheKeyMutation()'s ">="
          // semantics (see its comment) correctly allow BOTH of this
          // SAME operation's sequential mutations to apply when the
          // first one succeeds, while a genuinely superseded
          // `expectedGeneration` correctly fails BOTH.
          if (self->tryApplyCacheKeyMutation(cacheKey, expectedGeneration)) {
            self->m_cache.touchAfterNotModified(cacheKey, result->refreshedEtag,
                                                result->refreshedLastModified);
          }
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
          self->completeCacheReadOrQuarantine(operationId, stale, cacheKey,
                                              expectedGeneration,
                                              /*promoteOnSuccess=*/true);
          return;
        }

        if (!result->asset.has_value()) {
          // Defensive only: AssetNetworkFetcher never returns
          // notModified==false with an empty asset.
          self->completeCacheReadOrQuarantine(operationId, stale, cacheKey,
                                              expectedGeneration,
                                              /*promoteOnSuccess=*/false);
          return;
        }

        // The origin sent a fresh 200 body despite our conditional
        // headers (its content genuinely changed): replace the cached
        // entry and serve the new content. Gated by the CAS check
        // (review round-3 item 14), exactly like startCandidate()'s
        // fresh-200 path: if stale, still hand THIS operation's own
        // consumers the freshly-fetched bytes, just never publish them
        // over a cache key a newer operation has already superseded.
        AssetCache::CachedEntry fresh = toCachedEntry(*result->asset);
        if (self->tryApplyCacheKeyMutation(cacheKey, expectedGeneration)) {
          self->m_cache.store(cacheKey, fresh);
        }
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
