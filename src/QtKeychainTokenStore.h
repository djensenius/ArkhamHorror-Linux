#pragma once

#include "IKeychainJobFactory.h"
#include "ITokenStore.h"

#include <QObject>
#include <memory>
#include <unordered_map>

namespace Arkham {

// Production ITokenStore backed by QtKeychain (Secret Service/libsecret or
// KWallet on Linux). Stores exactly one token per canonical
// ServerProfile::profileId() under a single stable service name; the
// canonical profile UUID is the only per-entry identifier ever used, so
// server URLs, emails, passwords, tokens, and display names never appear in
// key names, diagnostics, or logs.
//
// QtKeychain's insecure plaintext fallback is never enabled (see
// QtKeychainJobFactory); an unsupported or locked backend is reported as a
// typed TokenStoreOutcome::Unavailable / AccessDenied failure, never a
// silent sign-out and never a fallback to QSettings, a file, or an
// environment variable.
//
// Every callback is queued (Qt::QueuedConnection) and guarded by a QPointer
// to |this|, so it is delivered asynchronously exactly once while the store
// is alive. The destructor disconnects and abandons every outstanding job
// (scheduling it for deleteLater() without invoking its callback), so no
// stale completion is ever delivered after destruction.
class QtKeychainTokenStore final : public QObject, public ITokenStore {
  Q_OBJECT
public:
  // Stable QtKeychain service name shared by every profile's token entry.
  static const QString &serviceName();

  // Production constructor: uses the real QtKeychainJobFactory.
  explicit QtKeychainTokenStore(QObject *parent = nullptr);

  // Test constructor: injects a fake IKeychainJobFactory so the adapter's
  // own sequencing and error-mapping logic can be exercised deterministically
  // without a real OS keyring.
  explicit QtKeychainTokenStore(std::unique_ptr<IKeychainJobFactory> factory,
                                QObject *parent = nullptr);

  ~QtKeychainTokenStore() override;

  void readToken(const QString &profileId,
                 const QString &expectedEndpointIdentity,
                 ResultCallback callback) override;
  void saveToken(const QString &profileId, const QString &token,
                 const QString &endpointIdentity,
                 ResultCallback callback) override;
  void deleteToken(const QString &profileId, ResultCallback callback) override;

private:
  struct PendingRead {
    std::unique_ptr<IKeychainReadJob> job;
    ResultCallback callback;
  };
  struct PendingWrite {
    std::unique_ptr<IKeychainWriteJob> job;
    ResultCallback callback;
  };
  struct PendingDelete {
    std::unique_ptr<IKeychainDeleteJob> job;
    ResultCallback callback;
  };

  // Emits |result| to |callback| exactly once, asynchronously, guarded by a
  // QPointer so a callback is never invoked after this store is destroyed.
  void emitAsync(ResultCallback callback, TokenStoreResult result);

  // Emits a TokenStoreOutcome::InvalidInput result asynchronously without
  // touching the secure-storage backend.
  void rejectInvalidInput(ResultCallback callback, QString diagnostic);

  std::unique_ptr<IKeychainJobFactory> m_factory;
  // std::unordered_map (not QHash) because PendingX holds a move-only
  // std::unique_ptr<IJob>; QHash requires its mapped type to be copyable
  // even when the map itself is never copied.
  std::unordered_map<IKeychainReadJob *, PendingRead> m_pendingReads;
  std::unordered_map<IKeychainWriteJob *, PendingWrite> m_pendingWrites;
  std::unordered_map<IKeychainDeleteJob *, PendingDelete> m_pendingDeletes;
};

} // namespace Arkham
