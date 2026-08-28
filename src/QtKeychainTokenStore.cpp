#include "QtKeychainTokenStore.h"

#include "QtKeychainJobFactory.h"

#include <QMetaObject>
#include <QPointer>
#include <QUuid>
#include <utility>

namespace Arkham {

namespace {

// Stable, non-identifying service name for every profile's token entry.
// Must never be derived from a server URL, email, or display name.
const QString kServiceName = QStringLiteral("app.arkhamhorror.auth.token");

// Parses and canonicalises a profile ID the same way
// ServerProfile::customWithId() does, rejecting unparseable and nil UUIDs.
// Returns an empty string on failure.
QString canonicalProfileId(const QString &profileId) {
  const QUuid parsed(profileId);
  if (parsed.isNull()) {
    return {};
  }
  return parsed.toString(QUuid::WithoutBraces);
}

TokenStoreOutcome mapReadError(QKeychain::Error err) {
  switch (err) {
  case QKeychain::NoError:
    return TokenStoreOutcome::Success;
  case QKeychain::EntryNotFound:
    return TokenStoreOutcome::NotFound;
  case QKeychain::AccessDeniedByUser:
  case QKeychain::AccessDenied:
    return TokenStoreOutcome::AccessDenied;
  case QKeychain::NoBackendAvailable:
  case QKeychain::NotImplemented:
    return TokenStoreOutcome::Unavailable;
  case QKeychain::CouldNotDeleteEntry:
  case QKeychain::OtherError:
    return TokenStoreOutcome::BackendError;
  }
  return TokenStoreOutcome::BackendError;
}

TokenStoreOutcome mapWriteError(QKeychain::Error err) {
  switch (err) {
  case QKeychain::NoError:
    return TokenStoreOutcome::Success;
  case QKeychain::AccessDeniedByUser:
  case QKeychain::AccessDenied:
    return TokenStoreOutcome::AccessDenied;
  case QKeychain::NoBackendAvailable:
  case QKeychain::NotImplemented:
    return TokenStoreOutcome::Unavailable;
  case QKeychain::EntryNotFound:
  case QKeychain::CouldNotDeleteEntry:
  case QKeychain::OtherError:
    return TokenStoreOutcome::BackendError;
  }
  return TokenStoreOutcome::BackendError;
}

// Deleting an entry that does not exist is treated as idempotent success:
// the post-condition ("no token stored for this profile") already holds.
TokenStoreOutcome mapDeleteError(QKeychain::Error err) {
  switch (err) {
  case QKeychain::NoError:
  case QKeychain::EntryNotFound:
    return TokenStoreOutcome::Success;
  case QKeychain::AccessDeniedByUser:
  case QKeychain::AccessDenied:
    return TokenStoreOutcome::AccessDenied;
  case QKeychain::NoBackendAvailable:
  case QKeychain::NotImplemented:
    return TokenStoreOutcome::Unavailable;
  case QKeychain::CouldNotDeleteEntry:
  case QKeychain::OtherError:
    return TokenStoreOutcome::BackendError;
  }
  return TokenStoreOutcome::BackendError;
}

// Static, secret-free diagnostic text. Never derived from
// QKeychain::Job::errorString(), which may echo backend-specific detail.
QString diagnosticFor(TokenStoreOutcome outcome) {
  switch (outcome) {
  case TokenStoreOutcome::Success:
    return QStringLiteral("secure-store operation completed successfully");
  case TokenStoreOutcome::NotFound:
    return QStringLiteral("no token is stored for this profile");
  case TokenStoreOutcome::AccessDenied:
    return QStringLiteral(
        "secure storage denied access to this entry; it may be locked");
  case TokenStoreOutcome::Unavailable:
    return QStringLiteral(
        "no supported secure-storage backend is available in this session");
  case TokenStoreOutcome::BackendError:
    return QStringLiteral("secure-storage backend reported an error");
  case TokenStoreOutcome::InvalidInput:
    return QStringLiteral("invalid input");
  }
  return QStringLiteral("secure-storage backend reported an error");
}

} // namespace

const QString &QtKeychainTokenStore::serviceName() { return kServiceName; }

QtKeychainTokenStore::QtKeychainTokenStore(QObject *parent)
    : QtKeychainTokenStore(std::make_unique<QtKeychainJobFactory>(), parent) {}

QtKeychainTokenStore::QtKeychainTokenStore(
    std::unique_ptr<IKeychainJobFactory> factory, QObject *parent)
    : QObject(parent), m_factory(std::move(factory)) {}

QtKeychainTokenStore::~QtKeychainTokenStore() {
  // Abandon every outstanding job without invoking its callback: destruction
  // must never deliver a stale completion. Disconnect first so an in-flight
  // finished() signal cannot reach a handler that references this object,
  // then release ownership and schedule deleteLater() instead of deleting
  // synchronously (safe even if a job is mid-emit on the call stack).
  for (auto it = m_pendingReads.begin(); it != m_pendingReads.end(); ++it) {
    QObject::disconnect(it->first, nullptr, this, nullptr);
    it->second.job.release()->deleteLater();
  }
  m_pendingReads.clear();
  for (auto it = m_pendingWrites.begin(); it != m_pendingWrites.end(); ++it) {
    QObject::disconnect(it->first, nullptr, this, nullptr);
    it->second.job.release()->deleteLater();
  }
  m_pendingWrites.clear();
  for (auto it = m_pendingDeletes.begin(); it != m_pendingDeletes.end(); ++it) {
    QObject::disconnect(it->first, nullptr, this, nullptr);
    it->second.job.release()->deleteLater();
  }
  m_pendingDeletes.clear();
}

void QtKeychainTokenStore::emitAsync(ResultCallback callback,
                                     TokenStoreResult result) {
  QPointer<QtKeychainTokenStore> self(this);
  QMetaObject::invokeMethod(
      this,
      [self, callback = std::move(callback), result = std::move(result)]() {
        if (self) {
          callback(result);
        }
      },
      Qt::QueuedConnection);
}

void QtKeychainTokenStore::rejectInvalidInput(ResultCallback callback,
                                              QString diagnostic) {
  emitAsync(std::move(callback),
            TokenStoreResult{TokenStoreOutcome::InvalidInput,
                             std::move(diagnostic), QString{}});
}

void QtKeychainTokenStore::readToken(const QString &profileId,
                                     ResultCallback callback) {
  const QString key = canonicalProfileId(profileId);
  if (key.isEmpty()) {
    rejectInvalidInput(std::move(callback),
                       QStringLiteral("profile ID must be a non-nil UUID"));
    return;
  }

  std::unique_ptr<IKeychainReadJob> job =
      m_factory->createReadJob(kServiceName, key);
  IKeychainReadJob *jobPtr = job.get();
  m_pendingReads.emplace(jobPtr,
                         PendingRead{std::move(job), std::move(callback)});

  connect(jobPtr, &IKeychainReadJob::finished, this, [this, jobPtr]() {
    auto it = m_pendingReads.find(jobPtr);
    if (it == m_pendingReads.end()) {
      return; // already handled (defensive; should not happen)
    }
    ResultCallback cb = it->second.callback;
    const QKeychain::Error err = jobPtr->error();
    QString token;
    const TokenStoreOutcome outcome = mapReadError(err);
    if (outcome == TokenStoreOutcome::Success) {
      token = jobPtr->textData();
    }
    it->second.job.release()->deleteLater();
    m_pendingReads.erase(it);
    emitAsync(std::move(cb),
              TokenStoreResult{outcome, diagnosticFor(outcome), token});
  });

  jobPtr->start();
}

void QtKeychainTokenStore::saveToken(const QString &profileId,
                                     const QString &token,
                                     ResultCallback callback) {
  const QString key = canonicalProfileId(profileId);
  if (key.isEmpty()) {
    rejectInvalidInput(std::move(callback),
                       QStringLiteral("profile ID must be a non-nil UUID"));
    return;
  }
  if (token.trimmed().isEmpty()) {
    rejectInvalidInput(
        std::move(callback),
        QStringLiteral("token must not be empty or whitespace-only"));
    return;
  }

  std::unique_ptr<IKeychainWriteJob> job =
      m_factory->createWriteJob(kServiceName, key);
  job->setTextData(token);
  IKeychainWriteJob *jobPtr = job.get();
  m_pendingWrites.emplace(jobPtr,
                          PendingWrite{std::move(job), std::move(callback)});

  connect(jobPtr, &IKeychainWriteJob::finished, this, [this, jobPtr]() {
    auto it = m_pendingWrites.find(jobPtr);
    if (it == m_pendingWrites.end()) {
      return;
    }
    ResultCallback cb = it->second.callback;
    const TokenStoreOutcome outcome = mapWriteError(jobPtr->error());
    it->second.job.release()->deleteLater();
    m_pendingWrites.erase(it);
    emitAsync(std::move(cb),
              TokenStoreResult{outcome, diagnosticFor(outcome), QString{}});
  });

  jobPtr->start();
}

void QtKeychainTokenStore::deleteToken(const QString &profileId,
                                       ResultCallback callback) {
  const QString key = canonicalProfileId(profileId);
  if (key.isEmpty()) {
    rejectInvalidInput(std::move(callback),
                       QStringLiteral("profile ID must be a non-nil UUID"));
    return;
  }

  std::unique_ptr<IKeychainDeleteJob> job =
      m_factory->createDeleteJob(kServiceName, key);
  IKeychainDeleteJob *jobPtr = job.get();
  m_pendingDeletes.emplace(jobPtr,
                           PendingDelete{std::move(job), std::move(callback)});

  connect(jobPtr, &IKeychainDeleteJob::finished, this, [this, jobPtr]() {
    auto it = m_pendingDeletes.find(jobPtr);
    if (it == m_pendingDeletes.end()) {
      return;
    }
    ResultCallback cb = it->second.callback;
    const TokenStoreOutcome outcome = mapDeleteError(jobPtr->error());
    it->second.job.release()->deleteLater();
    m_pendingDeletes.erase(it);
    emitAsync(std::move(cb),
              TokenStoreResult{outcome, diagnosticFor(outcome), QString{}});
  });

  jobPtr->start();
}

} // namespace Arkham
