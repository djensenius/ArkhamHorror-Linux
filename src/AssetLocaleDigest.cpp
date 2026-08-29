#include "AssetLocaleDigest.h"

#include "AssetLocaleDigestData.generated.h"

#include <QLatin1StringView>
#include <QSet>
#include <QString>
#include <QtAssert>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

QLatin1StringView categoryToken(AssetCategory category) {
  switch (category) {
  case AssetCategory::Card:
    return "card"_L1;
  case AssetCategory::InvestigatorPortrait:
    return "investigator_portrait"_L1;
  case AssetCategory::ChaosToken:
    return "chaos_token"_L1;
  case AssetCategory::SetIcon:
    return "set_icon"_L1;
  case AssetCategory::CampaignBox:
    return "campaign_box"_L1;
  case AssetCategory::SlotIcon:
    return "slot_icon"_L1;
  case AssetCategory::HomebrewCard:
    return "homebrew_card"_L1;
  case AssetCategory::HomebrewSet:
    return "homebrew_set"_L1;
  case AssetCategory::HomebrewBox:
    return "homebrew_box"_L1;
  }
  Q_UNREACHABLE_RETURN(QLatin1StringView());
}

QLatin1StringView sideToken(AssetSide side) {
  switch (side) {
  case AssetSide::Front:
    return "front"_L1;
  case AssetSide::Back:
    return "back"_L1;
  case AssetSide::AlternateFront:
    return "alternate_front"_L1;
  case AssetSide::ResolvedFront:
    return "resolved_front"_L1;
  case AssetSide::MutatedFront:
    return "mutated_front"_L1;
  }
  Q_UNREACHABLE_RETURN(QLatin1StringView());
}

// Encodes a variable-length field as its UTF-16 code-unit COUNT (not byte
// count) followed by a NUL separator, then the field itself. Composite
// keys built purely by concatenating variable-length fields (webLocale,
// identifier) back-to-back could otherwise collide across two distinct
// logical tuples if one field's suffix happens to look like the next
// field's prefix (the same class of bug fixed in
// AssetRequestCoordinator::canonicalOperationKey() for AssetKey
// coalescing) -- length-prefixing every field makes the composite
// injective regardless of field contents.
QString lengthPrefixed(const QString &field) {
  return QString::number(field.size()) + u'\0' + field;
}

// Builds (once, lazily, on first call -- C++11 guarantees this
// initialization is both thread-safe and happens at most once) an O(1)
// hash-set index over every (webLocale, category, identifier, side)
// tuple in AssetLocaleDigestData::kEntries, keyed by the same
// length-prefixed composite encoding hasLocalizedVariant() below queries
// with. contracts/asset-locale-digest.json's own provenance note
// documents this seed list as expected to be replaced with a full
// import later; building an index once up front (rather than
// linear-scanning kEntries on every call, which is only cheap while the
// seed list stays small) keeps hasLocalizedVariant() from becoming an
// O(N)-per-call hot-path bottleneck once that happens.
const QSet<QString> &localizedVariantIndex() {
  static const QSet<QString> index = [] {
    QSet<QString> built;
    built.reserve(
        static_cast<qsizetype>(std::size(AssetLocaleDigestData::kEntries)));
    for (const auto &entry : AssetLocaleDigestData::kEntries) {
      built.insert(lengthPrefixed(QString::fromLatin1(entry.webLocale)) +
                   lengthPrefixed(QString::fromLatin1(entry.category)) +
                   lengthPrefixed(QString::fromLatin1(entry.identifier)) +
                   lengthPrefixed(QString::fromLatin1(entry.side)));
    }
    return built;
  }();
  return index;
}

} // namespace

namespace AssetLocaleDigest {

QString webLocaleFor(const QString &isoLocale) {
  for (const auto &entry : AssetLocaleDigestData::kLocaleMap) {
    if (isoLocale == QLatin1StringView(entry.isoLocale)) {
      return QString::fromLatin1(entry.webLocale);
    }
  }
  return QString();
}

bool hasLocalizedVariant(const QString &webLocale, AssetCategory category,
                         const QString &identifier, AssetSide side) {
  if (webLocale.isEmpty()) {
    return false;
  }
  const QString key =
      lengthPrefixed(webLocale) + lengthPrefixed(categoryToken(category)) +
      lengthPrefixed(identifier) + lengthPrefixed(sideToken(side));
  return localizedVariantIndex().contains(key);
}

QString pinnedSourceJsonSha256() {
  return QString::fromLatin1(AssetLocaleDigestData::kSourceJsonSha256);
}

} // namespace AssetLocaleDigest

} // namespace Arkham
