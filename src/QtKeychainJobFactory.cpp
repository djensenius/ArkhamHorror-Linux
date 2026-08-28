#include "QtKeychainJobFactory.h"

#include <qtkeychain/keychain.h>

namespace Arkham {

namespace {

// Adapts a real QKeychain::ReadPasswordJob to IKeychainReadJob by
// composition (not inheritance): QKeychain's job classes are not designed
// to be subclassed, so the adapter owns a real job as a plain member
// subobject and forwards finished().
//
// m_job is constructed with a null QObject parent, never `this`. QObject's
// parent-child ownership model assumes children are heap-allocated: a
// parent's destructor (QObjectPrivate::deleteChildren(), invoked from
// ~QObject()) calls `delete` on every remaining child pointer. m_job is
// instead a member subobject of a job that is itself heap-allocated by the
// factory (see createReadJob() below) and owned by the caller via
// std::unique_ptr<IKeychainReadJob> -- registering it as a QObject child of
// `this` would risk that machinery attempting to `delete` a non-heap
// address (a double-free/heap-corruption class of bug), depending on
// exactly when relative to the wrapper's own destruction any pending
// Qt-internal bookkeeping runs. Passing nullptr instead removes this class
// of risk entirely: m_job's lifetime is governed solely by RealReadJob's
// own lifetime as an ordinary C++ member, with no parent-child
// relationship for Qt to manage.
//
// Separately, QKeychain::Job::emitFinished() calls this->deleteLater() on
// itself whenever autoDelete() is true, which QKeychain defaults to (see
// keychain.cpp: "autoDelete(true)" in Job's constructor and the
// "if (d->autoDelete) deleteLater();" in emitFinished()). QtKeychain's own
// jobs are meant to be used heap-allocated and self-destroying; left at its
// default, the deferred-delete event posted for m_job's address would fire
// against a non-heap-allocated address once this job completes -- the same
// class of bug as above, reproduced deterministically by this repo's
// kwallet_hardening test. setAutoDelete(false) is required so m_job's
// lifetime is governed solely by RealReadJob's own lifetime.
class RealReadJob final : public IKeychainReadJob {
public:
  RealReadJob(const QString &service, const QString &key)
      : m_job(service, nullptr) {
    m_job.setKey(key);
    m_job.setInsecureFallback(false); // never allow plaintext storage
    m_job.setAutoDelete(false); // m_job is a member subobject, not heap-owned
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
      : m_job(service, nullptr) {
    m_job.setKey(key);
    m_job.setInsecureFallback(false); // never allow plaintext storage
    // m_job is a member subobject, not heap-owned: null QObject parent
    // (see RealReadJob's class comment above) plus setAutoDelete(false)
    // together rule out any parent-child or deleteLater()-based
    // double-free/heap-corruption risk.
    m_job.setAutoDelete(false);
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
      : m_job(service, nullptr) {
    m_job.setKey(key);
    m_job.setInsecureFallback(false); // never allow plaintext storage
    // m_job is a member subobject, not heap-owned: null QObject parent
    // (see RealReadJob's class comment above) plus setAutoDelete(false)
    // together rule out any parent-child or deleteLater()-based
    // double-free/heap-corruption risk.
    m_job.setAutoDelete(false);
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
