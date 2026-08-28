#include "QtKeychainJobFactory.h"

#include <qtkeychain/keychain.h>

namespace Arkham {

namespace {

// Adapts a real QKeychain::ReadPasswordJob to IKeychainReadJob by
// composition (not inheritance): QKeychain's job classes are not designed
// to be subclassed, so the adapter owns a real job as a plain member
// subobject and forwards finished().
//
// m_job is constructed with a null QObject parent, never `this`, and with
// setAutoDelete(false) explicitly overriding QKeychain's default. These are
// two independent decisions, not one workaround for a single risk:
//
// - setAutoDelete(false) is the fix for a real, deterministic
//   double-free/heap-corruption risk: QKeychain::Job::emitFinished() calls
//   this->deleteLater() on itself whenever autoDelete() is true, which
//   QKeychain defaults to (see keychain.cpp: "autoDelete(true)" in Job's
//   constructor and "if (d->autoDelete) deleteLater();" in
//   emitFinished()). deleteLater() posts a deferred-delete event, targeting
//   m_job's address, into the event loop -- but m_job is a member
//   subobject, never heap-allocated on its own, so that deferred event
//   would eventually call operator delete on a non-heap address. This repo's
//   kwallet_hardening test reproduces the corruption deterministically when
//   autoDelete is left at its default; setAutoDelete(false) removes the
//   deleteLater() call entirely, so m_job's lifetime is governed solely by
//   RealReadJob's own lifetime as an ordinary C++ member.
// - The null QObject parent is a separate, narrower decision: it is NOT
//   fixing a double-free from the parent-child ownership machinery. If
//   `this` were passed as m_job's parent instead, C++ destroys members in
//   reverse declaration order before the base class's own destructor body
//   runs -- so m_job's destructor (a plain QObject destructor, which
//   unregisters the object from its parent's internal children list) would
//   already have run and removed m_job from that list before QObject's
//   base-class destructor (~QObject(), via
//   QObjectPrivate::deleteChildren()) ever got a chance to iterate the
//   remaining children and delete them. That ordering means a `this`
//   parent would not, on its own, cause deleteChildren() to double-delete
//   m_job. Passing nullptr instead is simply the more explicit choice:
//   m_job's ownership is already fully expressed by ordinary C++ member
//   lifetime, so registering it as a QObject child would only add
//   unnecessary parent-child bookkeeping with no corresponding ownership
//   benefit.
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
    // m_job is a member subobject, not heap-owned: setAutoDelete(false)
    // prevents QKeychain's own deleteLater() self-delete from targeting a
    // non-heap address (the real double-free/heap-corruption risk -- see
    // RealReadJob's class comment above for the full explanation); the
    // null QObject parent is a separate, simpler ownership choice that
    // makes m_job's plain-C++-member lifetime explicit rather than
    // registering an unnecessary QObject parent-child relationship.
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
    // m_job is a member subobject, not heap-owned: setAutoDelete(false)
    // prevents QKeychain's own deleteLater() self-delete from targeting a
    // non-heap address (the real double-free/heap-corruption risk -- see
    // RealReadJob's class comment above for the full explanation); the
    // null QObject parent is a separate, simpler ownership choice that
    // makes m_job's plain-C++-member lifetime explicit rather than
    // registering an unnecessary QObject parent-child relationship.
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
