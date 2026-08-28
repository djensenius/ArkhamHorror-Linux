#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include <qtkeychain/keychain.h>

namespace Arkham {

// Injectable seam over QtKeychain's per-operation job classes.
//
// QKeychain::ReadPasswordJob / WritePasswordJob / DeletePasswordJob are
// concrete (non-virtual) classes that talk to the real OS secure-storage
// backend, so they cannot be exercised deterministically in tests without a
// real keyring. These thin interfaces mirror only the subset of each job's
// API that QtKeychainTokenStore actually uses: construct via the factory,
// connect finished(), call start(), then read error()/errorString() (and
// textData() for a read) exactly once from the finished handler.
//
// QtKeychainJobFactory (production) wraps the real QtKeychain job classes.
// Tests inject a fake IKeychainJobFactory to drive QtKeychainTokenStore's
// actual sequencing and error-mapping logic without touching a real keyring.

class IKeychainReadJob : public QObject {
  Q_OBJECT
public:
  explicit IKeychainReadJob(QObject *parent = nullptr) : QObject(parent) {}
  ~IKeychainReadJob() override = default;

  virtual void start() = 0;
  [[nodiscard]] virtual QKeychain::Error error() const = 0;
  [[nodiscard]] virtual QString errorString() const = 0;
  [[nodiscard]] virtual QString textData() const = 0;

signals:
  // Emitted exactly once, asynchronously, after start().
  void finished();
};

class IKeychainWriteJob : public QObject {
  Q_OBJECT
public:
  explicit IKeychainWriteJob(QObject *parent = nullptr) : QObject(parent) {}
  ~IKeychainWriteJob() override = default;

  virtual void setTextData(const QString &data) = 0;
  virtual void start() = 0;
  [[nodiscard]] virtual QKeychain::Error error() const = 0;
  [[nodiscard]] virtual QString errorString() const = 0;

signals:
  void finished();
};

class IKeychainDeleteJob : public QObject {
  Q_OBJECT
public:
  explicit IKeychainDeleteJob(QObject *parent = nullptr) : QObject(parent) {}
  ~IKeychainDeleteJob() override = default;

  virtual void start() = 0;
  [[nodiscard]] virtual QKeychain::Error error() const = 0;
  [[nodiscard]] virtual QString errorString() const = 0;

signals:
  void finished();
};

// Creates jobs bound to a fixed service name (see QtKeychainTokenStore).
// Each create*Job() call returns a freshly constructed, unparented job ready
// to have its key set by the caller (job classes take the key via the
// service/key pair passed at creation, matching QtKeychain's own
// service+key addressing) and then start()ed.
class IKeychainJobFactory {
public:
  virtual ~IKeychainJobFactory() = default;

  [[nodiscard]] virtual std::unique_ptr<IKeychainReadJob>
  createReadJob(const QString &service, const QString &key) = 0;
  [[nodiscard]] virtual std::unique_ptr<IKeychainWriteJob>
  createWriteJob(const QString &service, const QString &key) = 0;
  [[nodiscard]] virtual std::unique_ptr<IKeychainDeleteJob>
  createDeleteJob(const QString &service, const QString &key) = 0;
};

} // namespace Arkham
