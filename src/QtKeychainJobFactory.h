#pragma once

#include "IKeychainJobFactory.h"

namespace Arkham {

// Production IKeychainJobFactory backed by real QKeychain::*PasswordJob
// instances. Every job created here has insecureFallback() explicitly set to
// false (QtKeychain's own default) so plaintext storage is never used, even
// if a future QtKeychain release changes its default.
class QtKeychainJobFactory final : public IKeychainJobFactory {
public:
  [[nodiscard]] std::unique_ptr<IKeychainReadJob>
  createReadJob(const QString &service, const QString &key) override;
  [[nodiscard]] std::unique_ptr<IKeychainWriteJob>
  createWriteJob(const QString &service, const QString &key) override;
  [[nodiscard]] std::unique_ptr<IKeychainDeleteJob>
  createDeleteJob(const QString &service, const QString &key) override;
};

} // namespace Arkham
