#pragma once

#include <QString>
#include <expected>

namespace Arkham {

// Strict three-component numeric version (major.minor.patch).
// Comparison is always numeric, never lexical:
//   0.1.9 < 0.1.11 — correct
//   "0.1.9" > "0.1.11" lexically — wrong, never used here.
struct ContractRevision {
  int major{0};
  int minor{0};
  int patch{0};

  [[nodiscard]] static std::expected<ContractRevision, QString>
  parse(QStringView str);
  [[nodiscard]] QString toString() const;

  auto operator<=>(const ContractRevision &) const = default;
};

} // namespace Arkham
