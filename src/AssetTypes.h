#pragma once

#include <QMetaType>
#include <QString>
#include <QUrl>
#include <cstdint>
#include <optional>
#include <utility>

namespace Arkham {

// Category of a native asset. Mirrors the web client's current canonical
// image routes (see AssetLocator.cpp for the exact path grammar per
// category). Every category is a *typed* enumerator, never an arbitrary
// caller-supplied path, so a caller can only ever ask for one of the asset
// families this client actually knows how to resolve and validate.
enum class AssetCategory {
  Card, // Player/encounter card art (see AssetSide for face).
  InvestigatorPortrait,
  ChaosToken,
  SetIcon,
  CampaignBox,
  SlotIcon,
  HomebrewCard,
  HomebrewSet,
  HomebrewBox,
};

// Side/variant of a Card asset. Ignored (must be Front) for every
// non-Card category: those categories have exactly one visual
// representation.
enum class AssetSide {
  Front,
  Back,
  AlternateFront, // Parallel/alternate-art front.
  ResolvedFront,  // The resolved side of a card that flips when resolved.
  MutatedFront,   // Sticker-mutated card art.
};

// Encoded image formats this client is willing to request/decode. SVG and
// arbitrary remote file types are explicitly out of scope (see issue #17).
enum class AssetFormat {
  Avif,
  Jpeg,
  Png,
};

[[nodiscard]] QString assetFormatExtension(AssetFormat format);
[[nodiscard]] QString assetFormatMimeType(AssetFormat format);

// Typed, discriminated failure reasons surfaced anywhere in the asset
// pipeline (locating, fetching, validating, or caching). Every failure a
// caller can observe is one of these -- never a bare QString or HTTP
// status alone -- so callers can react deterministically (e.g. "advance
// the candidate list" only for NotFound, never for Transport/Tls/Cancelled).
enum class AssetErrorCode {
  InvalidAssetBase,       // assetBase failed UrlValidator::validateCustomUrl(),
                          // or was never validated at all (default-constructed
                          // ValidatedAssetSource) -- see ValidatedAssetSource
                          // below for why the latter can no longer be a
                          // "revalidation" bypass.
  InvalidIdentifier,      // identifier failed the category's grammar.
  InvalidSideForCategory, // side is not Front for a non-Card category.
  InvalidSideForIdentifier, // side is structurally inapplicable to this
                            // specific identifier's shape (e.g. an
                            // AlternateFront request for an identifier not
                            // ending in a digit; see AssetLocator.cpp).
  InvalidHomebrewNamespace, // homebrewNamespace is missing/invalid for
                            // HomebrewCard, or non-empty for any category
                            // that must not carry one.
  InvalidMutationId, // mutationId is missing/invalid for MutatedFront, or
                     // non-empty for any other side.
  FormatMismatchForCategory, // key.format does not match the fixed,
                             // caller-non-configurable format the real CDN
                             // serves for this category (see
                             // AssetLocator::canonicalFormatFor()).
  NoCandidates,              // Locator produced zero candidates for this key.
  NotFound,                  // Every candidate returned a definitive 404.
  Transport,                 // Connection/TLS/generic network failure.
  RedirectRejected,    // A 3xx was returned; redirects are never followed.
  UnexpectedStatus,    // Any other non-2xx, non-404 status.
  ResponseTooLarge,    // Encoded body exceeded the configured byte cap.
  ContentTypeMismatch, // Declared Content-Type is not an accepted image type.
  MagicBytesMismatch,  // Content-Type claimed a format the bytes are not.
  DimensionTooLarge,   // A single dimension exceeded the configured cap.
  PixelBudgetExceeded, // width*height exceeded the configured cap.
  UnsupportedCodec,    // Bytes are valid but no installed Qt plugin decodes
                       // this format (e.g. AVIF plugin missing).
  MalformedImage,      // Bytes passed magic-byte sniffing but failed to
                       // decode (truncated/corrupt).
  Cancelled,           // Caller cancelled, or the last consumer went away.
  CacheCorrupt,        // A cached payload/metadata pair failed validation.
  ConditionalWithoutCachedBody, // A 304 arrived with no valid cached body.
  UnsupportedScheme, // URL scheme is not "http"/"https" (AssetNetworkFetcher
                     // is an HTTP(S)-only fetcher and fails closed on any
                     // other scheme, e.g. file://, rather than passing it
                     // through to QNetworkAccessManager).
};

struct AssetError {
  AssetErrorCode code{AssetErrorCode::Transport};
  QString message;
};

// Result of any asset-pipeline operation that can fail with a typed
// AssetError: either a value of T on success, or an AssetError on failure.
// Modelled after UrlValidator::UrlValidationResult (see UrlValidator.h)
// rather than the plain-QString ValueOrError<T>, because callers here must
// be able to switch on a discriminated AssetErrorCode (e.g. "advance the
// candidate list only for NotFound") rather than merely displaying a
// message.
template <typename T> class AssetOutcome {
public:
  AssetOutcome(T value) : m_value(std::move(value)) {}          // NOLINT
  AssetOutcome(AssetError error) : m_error(std::move(error)) {} // NOLINT

  [[nodiscard]] bool has_value() const noexcept { return m_value.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] const T &operator*() const { return *m_value; }
  [[nodiscard]] T &operator*() { return *m_value; }
  [[nodiscard]] const T *operator->() const { return &*m_value; }
  [[nodiscard]] T *operator->() { return &*m_value; }
  [[nodiscard]] const AssetError &error() const { return m_error; }

private:
  std::optional<T> m_value;
  AssetError m_error;
};

// An asset base URL that has been validated, once, against the EXACT raw
// caller-supplied string -- never against a QUrl already round-tripped
// through parsing. This matters because QUrl parsing itself is lossy in
// exactly the ways the shared ServerProfile/UrlValidator raw-authority
// grammar cares about (ambiguous numeric-loopback spellings collapse to a
// single canonical form, Unicode casefold/homoglyph tricks and control
// characters can be silently normalised or dropped): a `QUrl` field that a
// caller could freely construct and assign would let a hostile raw string
// be "laundered" through QUrl construction before this type ever saw it,
// making the base-URL policy effectively unenforceable no matter how
// faithfully AssetLocator re-validates the resulting QUrl.
//
// The type closes that gap structurally rather than defensively: the ONLY
// way to obtain an instance whose isValid() is true is fromRaw(), which
// runs the input through UrlValidator::validateCustomUrl() -- the exact
// same policy ServerProfile/NetworkAuthenticationClient use -- while the
// input is still the original raw QString. There is no public setter and
// no way to mutate an instance after construction. The default
// constructor exists only so AssetKey itself stays default-constructible
// (e.g. for a caller that forgot to populate assetBase at all); it always
// yields isValid() == false, which AssetLocator rejects with
// AssetErrorCode::InvalidAssetBase exactly as it would any other invalid
// base -- there is no "already validated, trust it" code path left to
// bypass.
class ValidatedAssetSource {
public:
  ValidatedAssetSource() = default;

  [[nodiscard]] static AssetOutcome<ValidatedAssetSource>
  fromRaw(const QString &rawInput);

  [[nodiscard]] bool isValid() const noexcept { return m_valid; }

  // Precondition: isValid(). The normalised base URL (scheme + host +
  // optional port + optional clean path prefix) exactly as
  // UrlValidator::validateCustomUrl() returned it.
  [[nodiscard]] const QUrl &normalizedUrl() const noexcept { return m_url; }

  [[nodiscard]] bool
  operator==(const ValidatedAssetSource &other) const noexcept {
    return m_valid == other.m_valid && m_url == other.m_url;
  }

private:
  explicit ValidatedAssetSource(QUrl url)
      : m_url(std::move(url)), m_valid(true) {}

  QUrl m_url;
  bool m_valid{false};
};

// Typed, immutable identity of one requested asset. Never carries an
// arbitrary path or URL: the concrete candidate URL(s) are derived from
// this descriptor by AssetLocator using a fixed, reviewed grammar per
// category.
//
// `assetBase` can only ever be genuinely valid if it was produced by
// ValidatedAssetSource::fromRaw() against the original raw input string
// (see ValidatedAssetSource above) -- there is no way to construct an
// AssetKey whose assetBase has silently bypassed that policy. It is
// included in every cache-namespace derivation so a hosted and a
// self-hosted server's cache entries can never collide even if they
// happen to request the same category/identifier/side/locale.
//
// `identifier` is validated per-category by AssetLocator's grammar (see
// AssetLocator.cpp): reject rather than sanitise separators, dot
// segments, control characters, userinfo-like constructs, and
// query/fragment injection. Never re-cased: identifiers are case-sensitive
// wherever the upstream source is (see AssetLocator.cpp for exactly which
// categories/fields are safely lower-cased).
//
// `locale` is a short BCP-47-ish language subtag (e.g. "it"); empty or
// "en" both mean "no localisation, use the English/default asset".
//
// `homebrewNamespace` is the homebrew campaign namespace segment used ONLY
// by AssetCategory::HomebrewCard (the real CDN nests homebrew card art
// under a per-campaign directory: "homebrew/{namespace}/cards/{code}...");
// it must be empty for every other category, including HomebrewSet/
// HomebrewBox, whose real paths reuse `identifier` itself as both the
// directory and file name and therefore need no separate field.
//
// `mutationId` is the arbitrary, backend-assigned per-instance sticker-
// mutation identifier used ONLY when `side == AssetSide::MutatedFront`
// (mirrors the real client's opaque `mutated` data field); it must be
// empty for every other side. It is validated with the same strict
// identifier grammar as `identifier` before it is ever placed in a URL
// path segment.
//
// `format` is validated, never trusted verbatim for path-building: the
// real CDN serves a single fixed format per category (e.g. always AVIF
// for card art, never caller-selectable), so AssetLocator rejects any key
// whose declared format does not match AssetLocator::canonicalFormatFor
// (category) with AssetErrorCode::FormatMismatchForCategory instead of
// silently overriding it -- a silent override would desync the URL
// extension from the Content-Type/magic-byte expectation
// AssetNetworkFetcher validates the response against.
struct AssetKey {
  ValidatedAssetSource assetBase;
  AssetCategory category{AssetCategory::Card};
  QString identifier;
  AssetSide side{AssetSide::Front};
  QString locale;
  QString homebrewNamespace;
  QString mutationId;
  AssetFormat format{AssetFormat::Jpeg};

  [[nodiscard]] bool operator==(const AssetKey &other) const noexcept {
    return assetBase == other.assetBase && category == other.category &&
           identifier == other.identifier && side == other.side &&
           locale == other.locale &&
           homebrewNamespace == other.homebrewNamespace &&
           mutationId == other.mutationId && format == other.format;
  }
};

// One concrete, fully-resolved URL AssetLocator proposes trying, in the
// order it should be tried. `locale` records which locale this specific
// candidate represents ("" for the English/default candidate) so a
// definitive-404 fallback can be told apart from an initial localized
// attempt; `isAlternateFrontFallback` marks the final alternate-front
// fallback candidate (only ever produced for Card/Front requests) so
// callers/tests can assert the fallback chain shape without re-deriving it
// from the URL text.
struct AssetCandidate {
  QUrl url;
  QString locale;
  bool isAlternateFrontFallback{false};

  [[nodiscard]] bool operator==(const AssetCandidate &other) const noexcept {
    return url == other.url && locale == other.locale &&
           isAlternateFrontFallback == other.isAlternateFrontFallback;
  }
};

} // namespace Arkham

Q_DECLARE_METATYPE(Arkham::AssetErrorCode)
