#include "QSettingsProfileStore.h"

#include <QSet>
#include <QUuid>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

QString kindToString(ServerProfileKind kind) {
  switch (kind) {
  case ServerProfileKind::HostedDefault:
    return QStringLiteral("hosted");
  case ServerProfileKind::Custom:
    return QStringLiteral("custom");
  }
  Q_UNREACHABLE_RETURN(QStringLiteral("custom"));
}

QString settingsStatusMessage(QSettings::Status status) {
  switch (status) {
  case QSettings::AccessError:
    return QStringLiteral("access denied");
  case QSettings::FormatError:
    return QStringLiteral("format error");
  default:
    return QStringLiteral("unknown error");
  }
}

bool isValidUuid(const QString &id) {
  return !id.isEmpty() && !QUuid(id).isNull();
}

} // namespace

QSettingsProfileStore::QSettingsProfileStore(const QString &organization,
                                             const QString &application)
    : m_ownedSettings(std::make_unique<QSettings>(QSettings::UserScope,
                                                  organization, application)),
      m_settings(m_ownedSettings.get()) {}

QSettingsProfileStore::QSettingsProfileStore(const QString &filePath)
    : m_ownedSettings(
          std::make_unique<QSettings>(filePath, QSettings::IniFormat)),
      m_settings(m_ownedSettings.get()) {}

ValueOrError<QList<ServerProfile>> QSettingsProfileStore::loadProfiles() const {
  if (m_settings->status() == QSettings::AccessError) {
    return failure(QStringLiteral("cannot read profiles: access denied"));
  }
  if (m_settings->status() == QSettings::FormatError) {
    return failure(QStringLiteral("cannot read profiles: file format error"));
  }

  QList<ServerProfile> profiles;
  QString parseError;
  const QString hostedId = ServerProfile::hostedDefault().profileId();

  const int count = m_settings->beginReadArray("Profiles"_L1);

  for (int i = 0; i < count && parseError.isEmpty(); ++i) {
    m_settings->setArrayIndex(i);

    const QString id = m_settings->value("id"_L1).toString();
    const QString kindStr = m_settings->value("kind"_L1).toString();

    if (kindStr == "hosted"_L1) {
      // Require the canonical hosted ID — do not silently reassign identity.
      if (id != hostedId) {
        parseError =
            QStringLiteral(
                "profile[%1]: hosted profile has wrong ID \"%2\"; expected "
                "\"%3\"")
                .arg(i)
                .arg(id, hostedId);
        break;
      }
      profiles.append(ServerProfile::hostedDefault());
    } else if (kindStr == "custom"_L1) {
      if (!isValidUuid(id)) {
        parseError =
            QStringLiteral("profile[%1]: invalid or missing profile ID \"%2\"")
                .arg(i)
                .arg(id);
        break;
      }
      const QString displayName =
          m_settings->value("displayName"_L1).toString();
      const QString baseUrl = m_settings->value("baseUrl"_L1).toString();

      if (baseUrl.isEmpty()) {
        parseError =
            QStringLiteral("profile[%1]: custom profile has no baseUrl").arg(i);
        break;
      }

      auto profileResult =
          ServerProfile::customWithId(id, displayName, baseUrl);
      if (!profileResult) {
        parseError = QStringLiteral("profile[%1]: corrupt data: %2")
                         .arg(i)
                         .arg(profileResult.error());
        break;
      }
      profiles.append(*profileResult);
    } else {
      parseError = QStringLiteral("profile[%1]: unknown kind \"%2\"")
                       .arg(i)
                       .arg(kindStr);
    }
  }

  m_settings->endArray(); // always balanced

  if (!parseError.isEmpty()) {
    return failure(parseError);
  }

  // Surface any lazy I/O failures that QSettings accumulates during reads.
  if (m_settings->status() != QSettings::NoError) {
    return failure(QStringLiteral("cannot read profiles: %1")
                       .arg(settingsStatusMessage(m_settings->status())));
  }

  // Reject duplicate effective profile IDs (e.g. two hosted entries, or a
  // persisted collision).
  QSet<QString> seenIds;
  for (int i = 0; i < profiles.size(); ++i) {
    const QString &pid = profiles[i].profileId();
    if (seenIds.contains(pid)) {
      return failure(QStringLiteral("duplicate profile ID \"%1\" at index %2")
                         .arg(pid)
                         .arg(i));
    }
    seenIds.insert(pid);
  }

  return profiles;
}

ValueOrError<QString> QSettingsProfileStore::loadSelectedProfileId() const {
  // Check for errors from a previous operation on this store.  QSettings
  // status is sticky, so a prior AccessError/FormatError (e.g. from
  // loadProfiles()) would otherwise cause us to return "no selection"
  // instead of a failure.
  if (m_settings->status() != QSettings::NoError) {
    return failure(QStringLiteral("cannot read selected profile ID: %1")
                       .arg(settingsStatusMessage(m_settings->status())));
  }

  const QVariant val = m_settings->value("Selection/profileId"_L1);

  // Check again: this may be the first read that actually opens the file.
  if (m_settings->status() != QSettings::NoError) {
    return failure(QStringLiteral("cannot read selected profile ID: %1")
                       .arg(settingsStatusMessage(m_settings->status())));
  }

  if (!val.isValid()) {
    return QString{}; // no selection persisted
  }
  const QString id = val.toString();
  if (id.isEmpty()) {
    return QString{}; // key present but empty — treat as no selection
  }
  const QUuid parsed(id);
  if (parsed.isNull()) {
    return failure(
        QStringLiteral("selected profile ID is not a valid UUID: \"%1\"")
            .arg(id));
  }
  // Return canonical form (strip any braces that might exist in older data).
  return parsed.toString(QUuid::WithoutBraces);
}

ValueOrError<bool>
QSettingsProfileStore::saveProfiles(const QList<ServerProfile> &profiles) {
  const QString hostedId = ServerProfile::hostedDefault().profileId();

  // Validate all profiles before touching storage.
  QSet<QString> seenIds;
  for (int i = 0; i < static_cast<int>(profiles.size()); ++i) {
    const ServerProfile &p = profiles[i];
    if (p.profileId().isEmpty()) {
      return failure(
          QStringLiteral(
              "profile[%1]: missing ID; use the factory methods to create "
              "profiles")
              .arg(i));
    }
    if (!p.isValid()) {
      return failure(QStringLiteral("profile[%1]: profile is invalid").arg(i));
    }
    if (p.displayName().trimmed().isEmpty()) {
      return failure(QStringLiteral("profile[%1]: blank display name").arg(i));
    }
    // Enforce kind/ID consistency.
    if (p.kind() == ServerProfileKind::HostedDefault &&
        p.profileId() != hostedId) {
      return failure(
          QStringLiteral("profile[%1]: hosted profile has inconsistent ID "
                         "\"%2\"")
              .arg(i)
              .arg(p.profileId()));
    }
    if (p.kind() == ServerProfileKind::Custom && p.profileId() == hostedId) {
      return failure(
          QStringLiteral("profile[%1]: custom profile must not carry the "
                         "hosted-default ID")
              .arg(i));
    }
    // Reject duplicate IDs.
    if (seenIds.contains(p.profileId())) {
      return failure(QStringLiteral("profile[%1]: duplicate profile ID \"%2\"")
                         .arg(i)
                         .arg(p.profileId()));
    }
    seenIds.insert(p.profileId());
  }

  // QSettings::status() is sticky: an earlier AccessError or FormatError
  // (e.g. from a failed loadProfiles() call on this store) remains set even
  // after successful in-memory writes.  Writing through a store that is
  // already in an error state can produce a success/failure mismatch — the
  // in-memory cache is updated but the post-sync status incorrectly reports
  // failure, or the file may be overwritten while the error is swallowed.
  // Refuse to touch storage until the store is in a clean state.
  if (m_settings->status() != QSettings::NoError) {
    return failure(
        QStringLiteral("cannot write profiles: pre-existing settings error: %1")
            .arg(settingsStatusMessage(m_settings->status())));
  }

  m_settings->beginWriteArray("Profiles"_L1, static_cast<int>(profiles.size()));
  for (int i = 0; i < static_cast<int>(profiles.size()); ++i) {
    m_settings->setArrayIndex(i);
    const ServerProfile &p = profiles[i];
    m_settings->setValue("id"_L1, p.profileId());
    m_settings->setValue("kind"_L1, kindToString(p.kind()));
    m_settings->setValue("displayName"_L1, p.displayName());
    m_settings->setValue("baseUrl"_L1, p.baseUrl().toString());
  }
  m_settings->endArray();
  m_settings->sync();

  if (m_settings->status() != QSettings::NoError) {
    return failure(QStringLiteral("failed to save profiles: %1")
                       .arg(settingsStatusMessage(m_settings->status())));
  }
  return true;
}

ValueOrError<bool>
QSettingsProfileStore::saveSelectedProfileId(const QString &id) {
  // Validate and canonicalize the UUID before any I/O so the canonical form
  // is ready to store and errors are caught early.
  QString canonicalId;
  if (!id.isEmpty()) {
    const QUuid parsed(id);
    if (parsed.isNull()) {
      return failure(
          QStringLiteral("selected profile ID is not a valid UUID: \"%1\"")
              .arg(id));
    }
    canonicalId = parsed.toString(QUuid::WithoutBraces);
  }

  // Refuse to touch storage if a prior operation left the store in an error
  // state.  See the matching guard in saveProfiles() for the full rationale.
  if (m_settings->status() != QSettings::NoError) {
    return failure(
        QStringLiteral(
            "cannot write selected profile ID: pre-existing settings error: %1")
            .arg(settingsStatusMessage(m_settings->status())));
  }

  if (canonicalId.isEmpty()) {
    m_settings->remove("Selection/profileId"_L1);
  } else {
    m_settings->setValue("Selection/profileId"_L1, canonicalId);
  }
  m_settings->sync();

  if (m_settings->status() != QSettings::NoError) {
    return failure(QStringLiteral("failed to save selected profile ID: %1")
                       .arg(settingsStatusMessage(m_settings->status())));
  }
  return true;
}

} // namespace Arkham
