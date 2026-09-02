#pragma once

#include "AssetNetworkFetcher.h"

#include <QUrl>

// Review round-5 item 1 (PR #18 cumulative review at 6bdc68cf):
// AssetFetchUrl::validate() is private, reachable only from
// AssetRequestCoordinator (this project's sole production caller) and
// from this dedicated test-support seam. Every test file that needs to
// construct an AssetFetchUrl directly -- rather than through a full
// AssetRequestCoordinator round-trip -- includes this header and calls
// AssetFetchUrlTestSupport::validate() instead of
// AssetFetchUrl::validate() directly.
//
// This is header-only and lives under tests/ (never linked into the
// production arkham_foundation static library or the arkham-horror
// executable), so it cannot become a production bypass: it exists purely
// to satisfy AssetFetchUrl's `friend class AssetFetchUrlTestSupport;`
// declaration for test translation units.
namespace Arkham {

class AssetFetchUrlTestSupport {
public:
  [[nodiscard]] static AssetOutcome<AssetFetchUrl> validate(const QUrl &url) {
    return AssetFetchUrl::validate(url);
  }
};

} // namespace Arkham
