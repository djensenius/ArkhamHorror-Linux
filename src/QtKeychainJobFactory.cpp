#include "QtKeychainJobFactory.h"

#include <qtkeychain/keychain.h>

namespace Arkham {

namespace {

// Adapts a real QKeychain::ReadPasswordJob to IKeychainReadJob by
// composition (not inheritance): QKeychain's job classes are not designed
// to be subclassed, so the adapter owns a real job as a child QObject and
// forwards finished().
class RealReadJob final : public IKeychainReadJob {
public:
  RealReadJob(const QString &service, const QString &key)
      : m_job(service, this) {
    m_job.setKey(key);
    m_job.setInsecureFallback(false); // never allow plaintext storage
    connect(&m_job, &QKeychain::Job::finished, this, &RealReadJob::finished);
  }

  void start() override { m_job.start(); }
  [[nodiscard]] QKeychain::Error error() const override {
    return m_job.error();
  }
  [[nodiscard]] QString errorString() const override {
    return m_job.errorString();
  }
  [[nodiscard]] QString textData() const override { return m_job.textData(); }

private:
  QKeychain::ReadPasswordJob m_job;
};

class RealWriteJob final : public IKeychainWriteJob {
public:
  RealWriteJob(const QString &service, const QString &key)
      : m_job(service, this) {
    m_job.setKey(key);
    m_job.setInsecureFallback(false); // never allow plaintext storage
    connect(&m_job, &QKeychain::Job::finished, this, &RealWriteJob::finished);
  }

  void setTextData(const QString &data) override { m_job.setTextData(data); }
  void start() override { m_job.start(); }
  [[nodiscard]] QKeychain::Error error() const override {
    return m_job.error();
  }
  [[nodiscard]] QString errorString() const override {
    return m_job.errorString();
  }

private:
  QKeychain::WritePasswordJob m_job;
};

class RealDeleteJob final : public IKeychainDeleteJob {
public:
  RealDeleteJob(const QString &service, const QString &key)
      : m_job(service, this) {
    m_job.setKey(key);
    m_job.setInsecureFallback(false); // never allow plaintext storage
    connect(&m_job, &QKeychain::Job::finished, this, &RealDeleteJob::finished);
  }

  void start() override { m_job.start(); }
  [[nodiscard]] QKeychain::Error error() const override {
    return m_job.error();
  }
  [[nodiscard]] QString errorString() const override {
    return m_job.errorString();
  }

private:
  QKeychain::DeletePasswordJob m_job;
};

} // namespace

std::unique_ptr<IKeychainReadJob>
QtKeychainJobFactory::createReadJob(const QString &service,
                                    const QString &key) {
  return std::make_unique<RealReadJob>(service, key);
}

std::unique_ptr<IKeychainWriteJob>
QtKeychainJobFactory::createWriteJob(const QString &service,
                                     const QString &key) {
  return std::make_unique<RealWriteJob>(service, key);
}

std::unique_ptr<IKeychainDeleteJob>
QtKeychainJobFactory::createDeleteJob(const QString &service,
                                      const QString &key) {
  return std::make_unique<RealDeleteJob>(service, key);
}

} // namespace Arkham
