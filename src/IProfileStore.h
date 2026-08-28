#pragma once

#include "ServerProfile.h"
#include "ValueOrError.h"

#include <QList>

namespace Arkham {

// Injectable abstraction for persisting the server profile list and the
// currently selected profile.  Selection is identified by a stable profile ID,
// not by an array index, so it survives reordering and deletion.
//
// Contract:
//   - loadProfiles() returns an empty list on first run (no data).
//   - loadProfiles() returns a failure if stored data is structurally corrupt
//     (unknown kind, unparseable URL, invalid profile ID) or if the backing
//     store reports an AccessError or FormatError.
//   - loadSelectedProfileId() returns an empty QString when nothing is
//     persisted (first run / no selection).
//   - loadSelectedProfileId() returns a failure when the backing store
//     reports an AccessError or FormatError, or when a stored non-empty value
//     is malformed or the null UUID.  It never interprets an unreadable store
//     as "no selection".
//   - saveProfiles() returns a failure if any profile is invalid or missing
//     an ID, if the backing store has a pre-existing error (sticky status),
//     or if the underlying storage sync fails.  Storage is not touched when
//     a pre-existing error is detected.
//   - saveSelectedProfileId() validates and canonicalizes its input, then
//     applies the same pre-existing-error guard and post-sync check as
//     saveProfiles().
//   - Credentials and tokens are never stored here.
class IProfileStore {
public:
  virtual ~IProfileStore() = default;

  [[nodiscard]] virtual ValueOrError<QList<ServerProfile>>
  loadProfiles() const = 0;

  // Returns empty QString (no selection) when nothing is persisted.
  // Returns a failure on AccessError/FormatError or when a stored non-empty
  // value is malformed or the null UUID.  Never silently treats an unreadable
  // store as "no selection".
  [[nodiscard]] virtual ValueOrError<QString> loadSelectedProfileId() const = 0;

  // Validates each profile (non-empty ID, isValid(), no duplicate IDs).
  // Returns failure without writing on any invalid entry or if the store has
  // a pre-existing error (sticky QSettings status from a prior operation).
  [[nodiscard]] virtual ValueOrError<bool>
  saveProfiles(const QList<ServerProfile> &profiles) = 0;

  // Passing an empty string removes the selection rather than writing it.
  // Validates and canonicalizes non-empty UUIDs before writing.
  // Returns failure on invalid input or a pre-existing store error.
  [[nodiscard]] virtual ValueOrError<bool>
  saveSelectedProfileId(const QString &id) = 0;
};

} // namespace Arkham
