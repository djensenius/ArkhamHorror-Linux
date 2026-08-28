#pragma once

#include "IProfileStore.h"

#include <QSettings>
#include <memory>

namespace Arkham {

// QSettings-backed implementation of IProfileStore.
//
// The two-argument production constructor creates a QSettings in UserScope.
// The single-argument constructor writes to the given INI file path and is
// intended for isolated test use only.
//
// Serialisation layout:
//   [Profiles]
//   1\id           = <uuid-string>
//   1\kind         = hosted | custom
//   1\displayName  = <string>
//   1\baseUrl      = <url>
//   size           = N
//   [Selection]
//   profileId      = <uuid-string>  (absent or empty = no selection)
//
// Hosted entries are always reconstructed via ServerProfile::hostedDefault()
// regardless of the stored displayName/baseUrl.  Custom entries are
// reconstructed via ServerProfile::customWithId(); an invalid ID or baseUrl
// is treated as corrupt data and causes loadProfiles() to return a failure.
//
// saveProfiles() validates every profile (non-empty ID, isValid()) before
// writing and checks QSettings::status() after sync, returning an explicit
// failure rather than silently succeeding on I/O errors.  Both save methods
// also check for a pre-existing sticky error (from a prior read or write on
// the same store) before touching storage, preventing success/failure
// mismatches caused by QSettings::status() being sticky after the first error.
class QSettingsProfileStore : public IProfileStore {
public:
  // Production constructor — uses QSettings::UserScope.
  explicit QSettingsProfileStore(const QString &organization,
                                 const QString &application);

  // Test constructor — uses QSettings::IniFormat at the given file path.
  explicit QSettingsProfileStore(const QString &filePath);

  [[nodiscard]] ValueOrError<QList<ServerProfile>>
  loadProfiles() const override;
  [[nodiscard]] ValueOrError<QString> loadSelectedProfileId() const override;
  [[nodiscard]] ValueOrError<bool>
  saveProfiles(const QList<ServerProfile> &profiles) override;
  [[nodiscard]] ValueOrError<bool>
  saveSelectedProfileId(const QString &id) override;

private:
  std::unique_ptr<QSettings> m_ownedSettings;
  QSettings *m_settings; // always non-null, points into m_ownedSettings
};

} // namespace Arkham
