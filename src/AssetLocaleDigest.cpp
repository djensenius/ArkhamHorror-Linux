#include "AssetLocaleDigest.h"

#include "AssetLocaleDigestData.generated.h"

#include <QLatin1StringView>
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
  const QLatin1StringView categoryTok = categoryToken(category);
  const QLatin1StringView sideTok = sideToken(side);
  for (const auto &entry : AssetLocaleDigestData::kEntries) {
    if (webLocale == QLatin1StringView(entry.webLocale) &&
        categoryTok == QLatin1StringView(entry.category) &&
        identifier == QLatin1StringView(entry.identifier) &&
        sideTok == QLatin1StringView(entry.side)) {
      return true;
    }
  }
  return false;
}

QString pinnedSourceJsonSha256() {
  return QString::fromLatin1(AssetLocaleDigestData::kSourceJsonSha256);
}

} // namespace AssetLocaleDigest

} // namespace Arkham
