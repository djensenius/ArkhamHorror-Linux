#include "QtKeychainTokenStore.h"

#include "QtKeychainJobFactory.h"
#include "TokenEnvelope.h"
#include "TokenValidation.h"

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
  case TokenStoreOutcome::BindingMismatch:
    return QStringLiteral(
        "stored credential does not match this profile's current server "
        "address and cannot be used");
  case TokenStoreOutcome::LegacyUnbound:
    return QStringLiteral(
        "stored credential predates this app's endpoint-binding format and "
        "cannot be trusted");
  case TokenStoreOutcome::Malformed:
    return QStringLiteral(
        "stored credential could not be parsed and cannot be trusted");
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
      [self, callback = std::move(callback),
       result = std::move(result)]() mutable {
        if (self) {
          std::move(callback)(std::move(result));
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
                                     const QString &expectedEndpointIdentity,
                                     ResultCallback callback) {
  const QString key = canonicalProfileId(profileId);
  if (key.isEmpty()) {
    rejectInvalidInput(std::move(callback),
                       QStringLiteral("profile ID must be a non-nil UUID"));
    return;
  }
  if (expectedEndpointIdentity.isEmpty()) {
    rejectInvalidInput(
        std::move(callback),
        QStringLiteral("expected endpoint identity must not be empty"));
    return;
  }

  std::unique_ptr<IKeychainReadJob> job =
      m_factory->createReadJob(kServiceName, key);
  IKeychainReadJob *jobPtr = job.get();
  m_pendingReads.emplace(jobPtr,
                         PendingRead{std::move(job), std::move(callback)});

  connect(jobPtr, &IKeychainReadJob::finished, this,
          [this, jobPtr, expectedEndpointIdentity]() {
            auto it = m_pendingReads.find(jobPtr);
            if (it == m_pendingReads.end()) {
              return; // already handled (defensive; should not happen)
            }
            ResultCallback cb = std::move(it->second.callback);
            const QKeychain::Error err = jobPtr->error();
            QString token;
            TokenStoreOutcome outcome = mapReadError(err);
            QString diagnostic;
            if (outcome == TokenStoreOutcome::Success) {
              const QString raw = jobPtr->textData();
              if (raw.trimmed().isEmpty()) {
                // saveToken() never persists an empty/whitespace-only payload,
                // so a backend that nonetheless returns one alongside a success
                // status indicates a corrupt/tampered entry (e.g. manually
                // edited outside this application), not a usable session or a
                // transient backend hiccup. Classified as Malformed (not
                // BackendError) so the coordinator's
                // required-delete-then-fresh-flow path can actually resolve it,
                // rather than retrying this exact same corrupt read forever
                // under a retryable-looking outcome. This check must run BEFORE
                // envelope parsing: parseTokenEnvelope() assumes a non-blank
                // input (see TokenEnvelope.h).
                outcome = TokenStoreOutcome::Malformed;
              } else {
                const TokenEnvelopeParseResult parsed = parseTokenEnvelope(raw);
                switch (parsed.outcome) {
                case TokenEnvelopeParseOutcome::Parsed:
                  // parseTokenEnvelope() itself now rejects any token
                  // failing isValidTokenContent() (see TokenValidation.h)
                  // as Malformed, so a Parsed outcome here always carries
                  // a usable token.
                  if (parsed.endpointIdentity == expectedEndpointIdentity) {
                    token = parsed.token;
                  } else {
                    outcome = TokenStoreOutcome::BindingMismatch;
                  }
                  break;
                case TokenEnvelopeParseOutcome::LegacyUnbound:
                  outcome = TokenStoreOutcome::LegacyUnbound;
                  break;
                case TokenEnvelopeParseOutcome::Malformed:
                  outcome = TokenStoreOutcome::Malformed;
                  break;
                }
              }
            }
            if (diagnostic.isEmpty()) {
              diagnostic = diagnosticFor(outcome);
            }
            it->second.job.release()->deleteLater();
            m_pendingReads.erase(it);
            emitAsync(std::move(cb),
                      TokenStoreResult{outcome, std::move(diagnostic),
                                       std::move(token)});
          });

  jobPtr->start();
}

void QtKeychainTokenStore::saveToken(const QString &profileId,
                                     const QString &token,
                                     const QString &endpointIdentity,
                                     ResultCallback callback) {
  const QString key = canonicalProfileId(profileId);
  if (key.isEmpty()) {
    rejectInvalidInput(std::move(callback),
                       QStringLiteral("profile ID must be a non-nil UUID"));
    return;
  }
  if (!isValidTokenContent(token)) {
    // See TokenValidation.h: this is the exact same shared check
    // parseTokenEnvelope() enforces on read, so a token rejected here can
    // never be the "writer persists something the reader later rejects"
    // gap -- it is simply never persisted in the first place.
    rejectInvalidInput(
        std::move(callback),
        QStringLiteral("token must be non-empty visible ASCII with no "
                       "spaces or control characters"));
    return;
  }
  if (endpointIdentity.isEmpty()) {
    rejectInvalidInput(std::move(callback),
                       QStringLiteral("endpoint identity must not be empty"));
    return;
  }

  std::unique_ptr<IKeychainWriteJob> job =
      m_factory->createWriteJob(kServiceName, key);
  job->setTextData(serializeTokenEnvelope(endpointIdentity, token));
  IKeychainWriteJob *jobPtr = job.get();
  m_pendingWrites.emplace(jobPtr,
                          PendingWrite{std::move(job), std::move(callback)});

  connect(jobPtr, &IKeychainWriteJob::finished, this, [this, jobPtr]() {
    auto it = m_pendingWrites.find(jobPtr);
    if (it == m_pendingWrites.end()) {
      return;
    }
    ResultCallback cb = std::move(it->second.callback);
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
    ResultCallback cb = std::move(it->second.callback);
    const TokenStoreOutcome outcome = mapDeleteError(jobPtr->error());
    it->second.job.release()->deleteLater();
    m_pendingDeletes.erase(it);
    emitAsync(std::move(cb),
              TokenStoreResult{outcome, diagnosticFor(outcome), QString{}});
  });

  jobPtr->start();
}

} // namespace Arkham
