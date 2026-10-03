#pragma once

#include <QByteArrayView>
#include <QString>

#include <optional>

namespace Arkham {

// Losslessly parses a /site-settings response body and returns assetHost only
// when it is present as a non-null JSON string. Additive fields are ignored;
// malformed JSON, non-object bodies, missing/null/non-string assetHost values,
// and raw-parser failures all decode to std::nullopt so callers can keep their
// configured/default asset host fallback.
[[nodiscard]] std::optional<QString>
decodeSiteSettingsAssetHost(QByteArrayView body);

} // namespace Arkham
