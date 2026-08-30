#include "AssetCache.h"

#include <QCache>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__linux__)
// statx()/struct statx are declared by glibc's own <sys/stat.h>, but
// STATX_MNT_ID itself is NOT visible through that header alone under a
// strict-ISO build: this project builds with
// `set(CMAKE_CXX_EXTENSIONS OFF)` (see CMakeLists.txt), i.e.
// `-std=c++23` rather than `-std=gnu++23`, so the compiler never
// implicitly defines `_GNU_SOURCE` -- and glibc's own
// bits/statx-generic.h (which is what actually declares STATX_MNT_ID)
// is gated behind `_GNU_SOURCE`/`_DEFAULT_SOURCE`. An earlier version
// of this file relied on plain `<sys/stat.h>` alone and was WRONG: the
// `#if defined(STATX_MNT_ID)` guard below silently evaluated false in
// this project's actual strict-ISO build configuration (even on a very
// new glibc/kernel that fully supports the feature at runtime),
// degrading every mount-id comparison to a bare st_dev check -- which
// treats a same-device bind mount as ordinary same-mount content
// instead of refusing to descend into it. Explicitly including
// <linux/stat.h> (the kernel UAPI header, unconditionally exposing the
// STATX_MNT_ID constant regardless of glibc feature-test macros, and
// safely coexisting with glibc's own <sys/stat.h> on any glibc new
// enough to define statx()/struct statx at all) closes that gap
// without weakening CMAKE_CXX_EXTENSIONS project-wide just for this
// one file. The `#if defined(STATX_MNT_ID)` guard below is kept
// regardless, so this still degrades to the pre-existing st_dev-only
// behaviour at compile time on any genuinely older/nonstandard header
// set lacking the kernel UAPI header entirely, and at RUN time on any
// older kernel that doesn't actually populate STATX_MNT_ID even though
// the headers declare it (see mountIdViaStatx()'s stx_mask check).
#include <linux/stat.h>
#include <sys/syscall.h>
// Round-9+ review (HIGH): openat2(2) (kernel 5.6+) lets every owned-
// directory-chain component be opened via ONE atomic syscall that the
// kernel itself refuses to complete (EXDEV/ELOOP) if the requested
// path would cross a mount boundary or resolve through a symlink --
// RESOLVE_NO_XDEV in particular gives a kernel-native "same mount"
// guarantee that does NOT depend on STATX_MNT_ID support at all (see
// MountIdentity's comment for why relying on st_dev alone is
// insufficient against a same-device bind mount), closing the "mount
// identity fallback to st_dev admits same-device bind" gap on any
// modern Linux kernel regardless of whether the separate statx()
// mount-id feature happens to be available. <linux/openat2.h> is the
// kernel UAPI header (declares `struct open_how` and the RESOLVE_*
// flags); it may be absent on an older build image's headers, or the
// glibc on the build machine may predate openat2's libc wrapper, so
// this is used opportunistically via a raw syscall(2) (exactly the
// existing statx()-hardening pattern above) with graceful,
// EQUIVALENT-STRENGTH degradation -- never a silently weaker one -- to
// the existing per-component fstatat(AT_SYMLINK_NOFOLLOW) + openat(
// O_NOFOLLOW) + mountIdentityMatches() sequence when openat2 itself is
// unavailable (compile-time absent header) or refused by the running
// kernel (ENOSYS), since that fallback sequence independently
// re-verifies no-follow and (when STATX_MNT_ID is available) mount
// continuity from scratch rather than trusting anything openat2 would
// have decided.
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#define ARKHAM_HAVE_OPENAT2_UAPI 1
#endif
#endif

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

constexpr int kMetadataFormatVersion = 1;
constexpr double kHighWaterMarkFraction = 0.90;
constexpr double kLowWaterMarkFraction = 0.75;

// Round-9+ review (MEDIUM): a Config with a negative disk-quota byte
// limit turns reapAndEnforceQuota()'s high/low-water-mark math negative
// too (see its `highWaterMark`/`lowWaterMark` computation below): the
// real, always-non-negative on-disk byte total then unconditionally
// exceeds that negative high-water mark, and the eviction loop's target
// (the negative low-water mark) can never be reached by evicting real,
// non-negative-sized entries -- so construction (and every later
// periodic sweep) destructively evicts every single entry it can, even
// though the caller most likely meant "some other, valid limit" or
// simply made a mistake, never "wipe the cache on every run". A negative
// memoryMaxCostBytes is the identical failure mode for the in-process
// QCache: QCache::setMaxCost() with a negative limit evicts every entry
// immediately, silently defeating the memory cache for this instance's
// entire lifetime. Both are treated as an invalid configuration and fail
// closed -- exactly like the existing symlink/root-mismatch failure
// modes -- rather than being discovered only once the destructive
// eviction has already run.
bool configHasValidDiskByteLimit(const AssetCache::Config &config) {
  return config.diskMaxBytes >= 0;
}

bool configHasValidMemoryByteLimit(const AssetCache::Config &config) {
  return config.memoryMaxCostBytes >= 0;
}

// The only legitimate writer of a cache payload is store(), which is only
// ever fed encoded bytes that already passed AssetNetworkFetcher's own
// hard incremental cap (`Limits::maxEncodedBytes`, 20 MiB). Mirroring
// that same ceiling here, independent of whatever either on-disk file
// claims about its own size, means a corrupted, truncated, or
// locally-planted payload/metadata pair can never trigger an unbounded
// read-back allocation -- see readExactSizeVerifiedRelative() below.
constexpr qint64 kMaxSinglePayloadBytesOnDisk = 20LL * 1024 * 1024;

// A legitimate metadata JSON file (see writeMetadata()) holds only a
// handful of short strings/numbers and is always well under 1 KiB in
// practice. This cache directory is locally writable and can contain a
// corrupted or maliciously-planted *.meta.json file; without an
// independent ceiling here, reading one via readAll() would be an
// unbounded allocation triggered purely by a file's on-disk size, before
// any of its content is even parsed. 64 KiB leaves generous headroom
// over any legitimate metadata payload while still bounding the read.
constexpr qint64 kMaxMetadataBytesOnDisk = 64 * 1024;

// Review round-3 item 8: an independent (deliberately NOT shared with
// AssetNetworkFetcher::Limits -- see kMaxSinglePayloadBytesOnDisk's
// comment for the same "mirror, don't couple" reasoning) sane upper bound
// on a single dimension read back from metadata. Real assets never
// remotely approach this; it exists purely so a corrupted/malicious
// metadata field can never reach the width/height cast below with a
// value that could overflow a later width*height multiplication.
constexpr qint64 kMaxDimensionPixelsOnDisk = 65536;

// The largest double value that can represent every integer exactly
// (2^53): the JSON number type is IEEE-754 double-precision, so ANY
// integer-valued field this cache itself ever legitimately writes (see
// writeMetadata()) that could plausibly exceed this bound must be
// persisted as a decimal STRING instead (see the accessSequence handling
// in readMetadata()/writeMetadata()) rather than a bare JSON number.
// Timestamp fields (insertedAtMs/lastAccessMs) are bounded against this
// same limit below purely as a "finite, integral, in-range" sanity gate
// -- not because a real timestamp could ever legitimately approach it.
constexpr double kMaxExactJsonIntegerDouble = 9007199254740992.0; // 2^53

// Review round-3 item 8: validates that `value` is a JSON number encoding
// a finite, integral, non-negative value no larger than `maxBound` --
// returns std::nullopt for anything else (a fractional value, NaN,
// +/-infinity, negative, oversized, or simply not a number at all).
// QJsonValue::toDouble() happily returns any of those for a corrupted or
// maliciously-planted metadata file; casting such a double DIRECTLY to a
// narrower integer type (as this file used to do for encodedSize/width/
// height) is undefined behaviour for an out-of-range value, not merely
// "wrong" -- this rejects all of that BEFORE any cast is ever performed,
// rather than trusting the cast itself to fail safely.
std::optional<qint64>
readBoundedNonNegativeIntegerField(const QJsonValue &value, qint64 maxBound) {
  if (!value.isDouble()) {
    return std::nullopt;
  }
  const double d = value.toDouble();
  if (!std::isfinite(d) || d < 0.0 || d > static_cast<double>(maxBound) ||
      d != std::trunc(d)) {
    return std::nullopt;
  }
  return static_cast<qint64>(d);
}

// Verifies `payloadFile`'s on-disk size against `expectedSize` (from
// metadata) and an absolute hard ceiling BEFORE ever calling read(), THEN
// reads at most `expectedSize + 1` bytes rather than trusting the
// earlier size() stat and calling readAll(): a corrupted, truncated, or
// locally-planted payload file can change size between the stat above
// and the read below (e.g. concurrent local tampering, or a special
// file whose size() cannot be trusted), so bounding the read itself --
// not just the pre-read stat -- is what actually prevents an unbounded
// allocation here. Reading exactly one byte more than `expectedSize`
// lets a file that grew past the expected size be rejected by comparing
// the actual byte count read against `expectedSize`, without ever
// allocating for more than `expectedSize + 1` bytes regardless of what
// the file claims or how large it has become. (This QFile-based version
// previously lived here; round-4/5 review item 3 replaces every
// production use of it with the fd-relative
// readExactSizeVerifiedRelative() below, which applies exactly this same
// size-drift-detection policy without ever reopening a path.)

#if defined(Q_OS_UNIX)
// Round-4/5 review item 3: the fd-relative I/O primitives below are the
// ONLY way this class ever reads, writes, renames, or unlinks a file
// once `m_rootFd` has been established at construction. Every one of
// them takes `dirFd` (always `m_rootFd`) and a bare filename, and
// resolves that name via openat()/fstatat()/renameat()/unlinkat() --
// NEVER by concatenating it onto `m_directory` and reopening the result
// via QFile/QSaveFile/QDir, which would re-resolve the path from the
// filesystem root and reintroduce exactly the TOCTOU window a retained
// descriptor exists to close (verifyRootAnchorLocked() becoming "only a
// witness" that checked a path an unrelated later open/rename/unlink
// call never actually used). `expectedDevice` (always `m_rootDevice`) is
// independently re-checked via fstat() on the FILE DESCRIPTOR actually
// opened (never a second path-based stat) before any content is
// trusted, rejecting a child that has been replaced by a different
// mounted filesystem (a bind mount, or any other device swapped in
// under the cache root after construction) -- see the class comment's
// mount-escape rationale.

// Verifies (via fstatat with AT_SYMLINK_NOFOLLOW -- an lstat
// equivalent) that `name` exists directly under `dirFd`. Returns the
// stat buffer on success; used by callers that need to classify an
// entry (regular file vs. directory vs. symlink node) without ever
// resolving through a final symlink component.
bool fstatRelativeNoFollow(int dirFd, const char *name, struct stat &outSt) {
  return fstatat(dirFd, name, &outSt, AT_SYMLINK_NOFOLLOW) == 0;
}

// Round-6 item 5: identifies a mounted filesystem more precisely than
// st_dev alone can. A bind mount of some OTHER directory onto a path
// underneath the cache root shares the exact same st_dev as its source
// filesystem -- bind mounts don't create a new block device, they just
// add another mount-table entry pointing at an already-mounted
// filesystem -- so an st_dev-only comparison (the pre-round-6 policy
// throughout this file) cannot distinguish "the cache root's own
// subtree" from "an unrelated directory bind-mounted into it from the
// SAME underlying filesystem". Linux 5.8+'s statx() STATX_MNT_ID field
// reports the kernel's actual per-mount identifier, which a same-device
// bind mount does NOT share with its host mount, closing that gap.
// `hasMountId` is false wherever this can't be determined (older
// kernel, older glibc without the STATX_MNT_ID constant, or a
// non-Linux platform) -- every comparison below falls back to the
// pre-existing st_dev-only policy in that case, which is a strictly
// weaker but still safe (fails closed on genuine device changes)
// guarantee than the full mount-id check.
struct MountIdentity {
  quint64 device{0};
  quint64 mountId{0};
  bool hasMountId{false};
};

#if defined(__linux__) && defined(STATX_MNT_ID)
std::optional<quint64> mountIdViaStatx(int dirFd, const char *name,
                                       int extraFlags) {
  struct statx buf {};
  if (statx(dirFd, name, extraFlags, STATX_MNT_ID, &buf) != 0) {
    return std::nullopt;
  }
  if ((buf.stx_mask & STATX_MNT_ID) == 0) {
    return std::nullopt; // kernel too old to actually populate this field
  }
  return buf.stx_mnt_id;
}
#endif

// MountIdentity for the entry named `name` directly under `dirFd`,
// given its already-obtained fstatat(..., AT_SYMLINK_NOFOLLOW) result
// `st` (used for the device number) -- makes an ADDITIONAL statx() call
// (also AT_SYMLINK_NOFOLLOW, also relative to `dirFd`, never a
// path re-resolved from the filesystem root) to recover the mount id
// when this platform/kernel supports it.
MountIdentity mountIdentityRelative(int dirFd, const char *name,
                                    const struct stat &st) {
  MountIdentity identity;
  identity.device = static_cast<quint64>(st.st_dev);
#if defined(__linux__) && defined(STATX_MNT_ID)
  if (const auto mountId = mountIdViaStatx(dirFd, name, AT_SYMLINK_NOFOLLOW)) {
    identity.mountId = *mountId;
    identity.hasMountId = true;
  }
#else
  Q_UNUSED(dirFd);
  Q_UNUSED(name);
#endif
  return identity;
}

// MountIdentity for an already-open file descriptor `fd` (an object
// that has ALREADY been opened -- e.g. via openat(..., O_NOFOLLOW) --
// so no further symlink-following risk applies here; this purely reads
// back what that descriptor refers to).
MountIdentity mountIdentityForFd(int fd) {
  MountIdentity identity;
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    return identity;
  }
  identity.device = static_cast<quint64>(st.st_dev);
#if defined(__linux__) && defined(STATX_MNT_ID)
  if (const auto mountId = mountIdViaStatx(fd, "", AT_EMPTY_PATH)) {
    identity.mountId = *mountId;
    identity.hasMountId = true;
  }
#endif
  return identity;
}

#if defined(ARKHAM_HAVE_OPENAT2_UAPI)
// Opportunistic, Linux-only atomic open of the single path component
// `name` relative to `dirFd`, refusing (via the KERNEL itself, in one
// syscall) both symlink resolution and any cross-mount traversal --
// RESOLVE_NO_SYMLINKS rejects a symlink node exactly like O_NOFOLLOW
// does, while RESOLVE_NO_XDEV additionally refuses to complete the open
// at all if `name` resolves onto a different mount/filesystem than
// `dirFd` itself, independent of (and strictly stronger than) any
// userspace st_dev/STATX_MNT_ID comparison performed afterward.
// RESOLVE_BENEATH is defense in depth for this call site specifically:
// `name` is always a single path segment with no embedded '/' (verified
// by the caller), so there is no ".." component for it to matter
// against, but it costs nothing extra and directly matches what the
// finding asks for. Returns -1 (errno set) on ANY failure, including
// "openat2 not supported by this kernel" (ENOSYS) -- the caller must
// treat -1 as "try the portable fallback", not as a definitive
// rejection, since an old kernel's ENOSYS says nothing about whether
// `name` is actually safe.
int openat2NoFollowNoXdev(int dirFd, const char *name, bool wantDirectory) {
  struct open_how how {};
  how.flags = O_RDONLY | O_CLOEXEC | (wantDirectory ? O_DIRECTORY : 0);
  how.resolve = RESOLVE_NO_SYMLINKS | RESOLVE_BENEATH | RESOLVE_NO_XDEV;
  const long result = ::syscall(SYS_openat2, dirFd, name, &how, sizeof(how));
  if (result < 0) {
    return -1;
  }
  return static_cast<int>(result);
}
#endif

// Opens the single path component `name` relative to `dirFd`, refusing
// a symlink node and (when the running kernel supports it) refusing any
// cross-mount traversal atomically via openat2() -- see
// openat2NoFollowNoXdev()'s comment. Falls back to plain
// openat(O_NOFOLLOW) (the pre-existing, portable mechanism, still
// independently re-verified by the caller's own
// fstatat()+mountIdentityMatches() sequence) on any non-Linux platform
// or when the running kernel refuses openat2 itself (old kernel): that
// fallback is an EQUIVALENT-STRENGTH, not weaker, guarantee for
// symlinks (O_NOFOLLOW), and for mount continuity it is exactly the
// pre-existing STATX_MNT_ID-when-available / st_dev-only-otherwise
// policy this file has always documented -- never silently downgraded
// any further by this function.
int openDirectoryComponentNoFollow(int dirFd, const char *name) {
#if defined(ARKHAM_HAVE_OPENAT2_UAPI)
  const int viaOpenat2 = openat2NoFollowNoXdev(dirFd, name, true);
  if (viaOpenat2 >= 0) {
    return viaOpenat2;
  }
#endif
  return openat(dirFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

// True iff `actual` names the same mount as `expected`: st_dev must
// always match; when BOTH sides have a kernel mount id recorded, that
// must match too (closing the same-device-bind-mount gap described
// above). When either side lacks a mount id (older kernel/platform),
// this degrades to the st_dev-only comparison -- see MountIdentity's
// comment for why that is a documented, narrower guarantee on such
// platforms rather than a silent full bypass.
bool mountIdentityMatches(const MountIdentity &actual,
                          const MountIdentity &expected) {
  if (actual.device != expected.device) {
    return false;
  }
  if (actual.hasMountId && expected.hasMountId) {
    return actual.mountId == expected.mountId;
  }
  return true;
}

// Opens `name` relative to `dirFd` for reading, verifying (via fstat on
// the RESULTING DESCRIPTOR -- never a second path-based stat) that what
// was actually opened is a regular file on the SAME mount as
// `expectedMount`. O_NOFOLLOW means a symlink node is refused outright
// (ELOOP) rather than silently followed; the mount check rejects a
// child that resolves onto a different mounted filesystem than the
// cache root itself. Returns -1 (with no fd left open) on any failure.
int openRegularNoFollowRelative(int dirFd, const QByteArray &nameUtf8,
                                const MountIdentity &expectedMount) {
  const int fd =
      openat(dirFd, nameUtf8.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      !mountIdentityMatches(mountIdentityForFd(fd), expectedMount)) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// A filesystem-safe, collision-resistant temporary filename for an
// atomic write of `finalName`, drawn from QRandomGenerator::system() --
// a real OS entropy source (unlike the seeded, reproducible
// QRandomGenerator::global()) -- so two writers targeting the same
// final name can never collide on the temp name itself, even though
// this class's own m_mutex already serializes access from within one
// process.
QString temporaryNameFor(const QString &finalName) {
  quint64 entropy[2] = {0, 0};
  QRandomGenerator::system()->fillRange(entropy, 2);
  return u'.' + finalName + ".tmp-"_L1 + QString::number(entropy[0], 16) +
         QString::number(entropy[1], 16);
}

// The fd-relative equivalent of QSaveFile: writes `bytes` to a brand-new
// temp file relative to `dirFd`, optionally fsyncs its content (see
// `durable`), then atomically renames it onto `finalName` (also
// relative to `dirFd`, via renameat -- never a path re-resolved from
// the filesystem root). QSaveFile itself has no API to target an
// already-open directory descriptor, only a path, which is exactly the
// "reopens the path" gap review round-4/5 item 3 requires closing. On
// ANY failure, a temp file that was actually created is unlinked before
// returning false, so a failed write never leaves debris behind for the
// reap sweep to have to clean up later (though it safely would).
bool writeFileAtomicRelative(int dirFd, const QString &finalName,
                             const QByteArray &bytes, bool durable) {
  const QString tempName = temporaryNameFor(finalName);
  const QByteArray tempNameUtf8 = tempName.toUtf8();
  const int fd =
      openat(dirFd, tempNameUtf8.constData(),
             O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    return false;
  }
  const char *data = bytes.constData();
  qint64 remaining = bytes.size();
  bool writeOk = true;
  while (remaining > 0) {
    const ssize_t n = ::write(fd, data, static_cast<size_t>(remaining));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      writeOk = false;
      break;
    }
    data += n;
    remaining -= n;
  }
  if (writeOk && durable) {
    writeOk = ::fsync(fd) == 0;
  }
  ::close(fd);
  if (!writeOk) {
    unlinkat(dirFd, tempNameUtf8.constData(), 0);
    return false;
  }
  const QByteArray finalNameUtf8 = finalName.toUtf8();
  if (renameat(dirFd, tempNameUtf8.constData(), dirFd,
               finalNameUtf8.constData()) != 0) {
    unlinkat(dirFd, tempNameUtf8.constData(), 0);
    return false;
  }
  return true;
}

// Reads at most `maxBytes + 1` bytes of `name` relative to `dirFd`,
// rejecting anything that isn't a regular, same-device file (see
// openRegularNoFollowRelative()) and anything whose actual read byte
// count exceeds `maxBytes`. Used for manifest/metadata reads, where the
// exact expected size isn't known ahead of time -- only a sane upper
// bound.
std::optional<QByteArray>
readBoundedRelative(int dirFd, const MountIdentity &expectedMount,
                    const QString &name, qint64 maxBytes) {
  const QByteArray nameUtf8 = name.toUtf8();
  const int fd = openRegularNoFollowRelative(dirFd, nameUtf8, expectedMount);
  if (fd < 0) {
    return std::nullopt;
  }
  QByteArray buffer;
  buffer.resize(static_cast<qsizetype>(maxBytes + 1));
  qint64 total = 0;
  bool ok = true;
  while (total < buffer.size()) {
    const ssize_t n = ::read(fd, buffer.data() + total,
                             static_cast<size_t>(buffer.size() - total));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      ok = false;
      break;
    }
    if (n == 0) {
      break; // EOF
    }
    total += n;
  }
  ::close(fd);
  if (!ok || total > maxBytes) {
    return std::nullopt;
  }
  buffer.resize(static_cast<qsizetype>(total));
  return buffer;
}

// Reads exactly `expectedSize` bytes of `name` relative to `dirFd` (plus
// one extra probe byte, applying the same size-drift-detection policy
// documented above at readBoundedRelative()'s docstring predecessor: a
// file that grew past `expectedSize` between an earlier stat and this
// read is rejected by the resulting byte count, never trusted based on
// a stale size() alone).
std::optional<QByteArray>
readExactSizeVerifiedRelative(int dirFd, const MountIdentity &expectedMount,
                              const QString &name, qint64 expectedSize,
                              qint64 hardCap) {
  if (expectedSize < 0 || expectedSize > hardCap) {
    return std::nullopt;
  }
  const std::optional<QByteArray> bytes =
      readBoundedRelative(dirFd, expectedMount, name, expectedSize + 1);
  if (!bytes || bytes->size() != expectedSize) {
    return std::nullopt;
  }
  return bytes;
}

// unlinkat relative to `dirFd`; tolerates an already-absent file (ENOENT)
// as success, exactly like QFile::remove()'s callers in this file
// already treat a missing file as "nothing left to do".
bool removeFileRelative(int dirFd, const QString &name) {
  const QByteArray nameUtf8 = name.toUtf8();
  return unlinkat(dirFd, nameUtf8.constData(), 0) == 0 || errno == ENOENT;
}

// Opens a BRAND NEW file description for the same directory `dirFd`
// already refers to, via `openat(dirFd, ".", ...)` -- resolved entirely
// through the fd (never a path, so it cannot be tricked into a
// different object even if some path once used to open `dirFd` has
// since been replaced) -- rather than `dup(dirFd)`. This distinction
// matters: POSIX `dup()` shares the underlying open file description,
// INCLUDING its current read/seek offset, with the fd it was duplicated
// from. A directory's "read position" for repeated readdir() calls is a
// property of that shared offset, so two sequential dup()-based
// listings of the SAME retained `dirFd` (e.g. one during
// reapAndEnforceQuota()'s full sweep, immediately followed by another
// inside deleteEntry()'s prefix scan while still holding `m_rootFd`)
// would silently observe whatever position the FIRST listing already
// advanced the shared offset to -- typically EOF -- causing the second
// listing to spuriously enumerate zero entries. `openat(dirFd, ".")`
// instead produces an independent, freshly-rewound file description
// every time, exactly as if the directory had been opened again by
// path, without ever re-resolving by path.
int openFreshHandleToSameDirectory(int dirFd) {
  return openat(dirFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

// Lists every immediate entry name directly under `dirFd` whose name
// begins with `prefix`, via a fresh directory handle (see
// openFreshHandleToSameDirectory()'s comment; never consuming the
// caller's own `dirFd`, and never sharing its read offset). Used by
// deleteEntry()'s name-prefix sweep, replacing a QDir::entryList() glob
// that would otherwise re-resolve `m_directory` by path.
//
// Round-7/8 item 6 (MEDIUM): returns std::nullopt when the enumeration
// itself could not be completed at all -- openat()/fdopendir() failure,
// or a readdir() call that fails partway through a scan (readdir()
// returns nullptr on BOTH a genuine end-of-directory and a failure;
// only checking `errno` after the loop distinguishes the two) -- rather
// than silently returning an empty (or truncated) list indistinguishable
// from "genuinely nothing matched". A caller that treated those two
// outcomes the same way would report a key's on-disk footprint as fully
// reclaimed purely because a transient, unrelated enumeration failure
// happened to occur, when files it would have named may still be
// sitting on disk, occupying real quota, entirely untouched.
std::optional<QStringList> listNamesWithPrefixRelative(int dirFd,
                                                       const QString &prefix) {
  const int freshFd = openFreshHandleToSameDirectory(dirFd);
  if (freshFd < 0) {
    return std::nullopt;
  }
  DIR *dirStream = fdopendir(freshFd);
  if (!dirStream) {
    ::close(freshFd);
    return std::nullopt;
  }
  QStringList names;
  errno = 0;
  while (struct dirent *entry = readdir(dirStream)) {
    if (qstrcmp(entry->d_name, ".") == 0 || qstrcmp(entry->d_name, "..") == 0) {
      errno = 0;
      continue;
    }
    const QString name = QString::fromUtf8(entry->d_name);
    if (name.startsWith(prefix)) {
      names.append(name);
    }
    errno = 0;
  }
  const bool readdirFailedPartway = errno != 0;
  closedir(dirStream); // also closes freshFd
  if (readdirFailedPartway) {
    return std::nullopt;
  }
  return names;
}

// One immediate entry under a directory, classified via a no-follow
// fstatat -- reports the entry's OWN type, never resolving through a
// final symlink component.
struct RelativeDirEntry {
  QString name;
  bool isSymlinkNode{false};
  bool isDirectory{false};
  qint64 sizeBytes{0};
};

// Lists every immediate entry directly under `dirFd` (via a fresh
// directory handle -- see openFreshHandleToSameDirectory()'s comment,
// never consuming the caller's own `dirFd` or sharing its read offset),
// classifying each one with fstatat(..., AT_SYMLINK_NOFOLLOW). Replaces
// reapAndEnforceQuota()'s former QDir::entryList() listing, which
// re-resolved `m_directory` by path for every call.
std::vector<RelativeDirEntry> listAllEntriesRelative(int dirFd) {
  std::vector<RelativeDirEntry> entries;
  const int freshFd = openFreshHandleToSameDirectory(dirFd);
  if (freshFd < 0) {
    return entries;
  }
  DIR *dirStream = fdopendir(freshFd);
  if (!dirStream) {
    ::close(freshFd);
    return entries;
  }
  errno = 0;
  while (struct dirent *de = readdir(dirStream)) {
    if (qstrcmp(de->d_name, ".") == 0 || qstrcmp(de->d_name, "..") == 0) {
      errno = 0;
      continue;
    }
    struct stat st {};
    if (fstatat(dirFd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
      RelativeDirEntry entry;
      entry.name = QString::fromUtf8(de->d_name);
      entry.isSymlinkNode = S_ISLNK(st.st_mode);
      entry.isDirectory = S_ISDIR(st.st_mode);
      entry.sizeBytes = static_cast<qint64>(st.st_size);
      entries.push_back(std::move(entry));
    }
    errno = 0;
  }
  closedir(dirStream); // also closes freshFd
  return entries;
}
#endif // Q_OS_UNIX

// A 64-character lowercase hex SHA-256 string -- the shape of both a
// cache key (cacheKeyFor()) and a generation identifier (a payload's own
// content hash): both are validated against this exact same pattern
// before ever becoming part of a filesystem path.
const QRegularExpression &validKeyPattern() {
  static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
  return re;
}

// Review item 8: on-disk filename shapes for the generation/manifest
// layout described in AssetCache.h. Anything found in the cache
// directory during a sweep that matches none of these three shapes (a
// QSaveFile crash artifact, debris from an old format version, etc.) is
// a stray leftover and removed outright -- this directory is exclusively
// owned and fully managed by AssetCache, so that is always safe.
const QRegularExpression &manifestNamePattern() {
  static const QRegularExpression re(
      QStringLiteral("^([0-9a-f]{64})\\.manifest\\.json$"));
  return re;
}
const QRegularExpression &generationPayloadNamePattern() {
  static const QRegularExpression re(
      QStringLiteral("^([0-9a-f]{64})\\.([0-9a-f]{64})\\.bin$"));
  return re;
}
const QRegularExpression &generationMetadataNamePattern() {
  static const QRegularExpression re(
      QStringLiteral("^([0-9a-f]{64})\\.([0-9a-f]{64})\\.meta\\.json$"));
  return re;
}

// (fsyncSaveFileBeforeCommit()/fsyncDirectory() previously lived here.
// Round-4/5 review item 3 replaces every QSaveFile-based write with
// writeFileAtomicRelative() above, which fsyncs its own file content
// internally before its renameat() -- and the equivalent "fsync the
// directory that just received a rename" step is now a direct
// `::fsync(m_rootFd)` call (fsyncRootLocked(), defined with the rest of
// this class's members below) rather than a function that reopens
// `m_directory` by path, which was itself part of the "witness only"
// gap this change closes.)

// All disk-touching public entry points (lookupDisk(), store(),
// touchAfterNotModified()) accept a caller-supplied key and use it,
// unescaped, to form filesystem paths via payloadPath()/metadataPath().
// AssetCache's own callers always pass a key produced by cacheKeyFor()
// (a SHA-256 hex digest), which can never contain a path separator or
// "..", but this is a public API -- nothing in the type system prevents
// a future or misbehaving caller from forwarding an arbitrary string.
// Rejecting anything that doesn't match the exact expected shape here,
// at every entry point that turns a key into a path, closes that off
// completely rather than relying on every call site to keep passing
// keys that happen to be safe.
bool isValidKey(const QString &key) {
  return validKeyPattern().match(key).hasMatch();
}

// (defaultCacheDirectory() previously lived here, returning
// QStandardPaths::writableLocation(CacheLocation) + "/assets/v1" as a
// single opaque string. Round-6 item 5's AssetCache::AssetCache() needs
// the OS-provided base and this application's own "assets"/"v1" suffix
// as SEPARATE values -- see its comment -- so that logic is now inlined
// there directly instead.)

#if defined(Q_OS_UNIX)
// Round-6 item 5: opens `trustedAnchorPath` via ordinary, O_NOFOLLOW-on-
// the-leaf-only resolution (exactly the pre-round-6 root-open policy),
// then walks every component of `ownedSuffixComponents` on top of it
// one path segment at a time via fstatat(AT_SYMLINK_NOFOLLOW) +
// openat(O_NOFOLLOW), rejecting the WHOLE resolution if ANY of those
// suffix components turns out to be a symlink.
//
// The split between "trusted anchor" and "owned suffix" is deliberate
// and load-bearing: `trustedAnchorPath` is always either (a) the
// OS/desktop-environment-provided cache base directory
// (QStandardPaths::CacheLocation) when this cache uses its default
// location, or (b) an entire caller-supplied Config::directory when one
// is explicitly configured -- in both cases, a path this application
// did not itself invent and whose provenance is a configuration input,
// not local-attacker-writable surface, exactly like the pre-round-6
// policy already trusted (avoids re-litigating long-standing,
// legitimately-symlinked OS conventions this application has no part
// in creating and does not own, e.g. macOS's classic `/var` ->
// `/private/var`, or a Linux distribution's /usr-merge symlinks --
// which caused genuine regressions in an earlier version of this fix
// that instead walked no-follow from the filesystem root). Anything
// named in `ownedSuffixComponents`, by contrast (the "assets/v1"
// this class itself appends to the OS cache base, in the default-
// location case), is a subtree this application EXCLUSIVELY owns and
// creates; nothing else should ever legitimately place a symlink OR a
// bind mount there, so every component of it is verified no-follow AND
// mount-identity-continuous with the trusted anchor, and any MISSING
// component is created directly here via mkdirat() -- never via
// QDir::mkpath(), which this function's caller (the constructor) no
// longer invokes at all. QDir::mkpath() is a plain path-based `mkdir
// -p` with no symlink-awareness whatsoever: given a path string, it
// re-resolves and creates each ancestor from the filesystem root on
// every call, so it would silently create whatever trailing components
// don't yet exist THROUGH an attacker-planted symlink for an ancestor
// one or more levels above the final leaf -- producing a perfectly
// ordinary, non-symlink leaf directory living wherever that symlink
// pointed (round-7/8 review: "destructively recovers foreign dir"),
// which a single final O_NOFOLLOW open could never detect after the
// fact. Creating each owned-suffix component via mkdirat() relative to
// the already-open, already-verified PARENT fd has no such path
// re-resolution step at all: there is no path string for an attacker's
// symlink to be substituted into.
//
// Round-7/8 review ("root-component loop doesn't enforce mount
// identity"): a bind mount planted for one of these owned-suffix path
// segments shares the SAME kernel mount table depth as an ordinary
// subdirectory would, and previously nothing in this loop ever compared
// device/mount identity between the anchor and each subsequent step --
// only the FINAL resulting fd's mount identity was recorded (by the
// constructor, for ONGOING verifyRootAnchorLocked() comparisons), never
// checked against the anchor it was supposed to be nested under in the
// first place. Every step below now requires mountIdentityMatches()
// against the anchor's own MountIdentity (captured once, immediately
// after opening it), degrading to the same st_dev-only comparison
// mountIdentityMatches() already documents when mount-id support is
// unavailable on this platform/kernel -- rejecting the whole resolution
// outright (never silently continuing) the instant any component
// resolves onto a different mount than the anchor itself.
// Round-9+ review: `allowCreateMissingComponents` lets a caller that
// must NEVER auto-create any part of a caller-supplied custom
// directory (see resolveTrustedDirectoryNoFollow()'s comment) reuse
// this exact same walker in pure-verification mode -- a missing
// component is then a hard rejection (std::nullopt) rather than an
// mkdirat() call, with no duplicated fstatat/openat logic between the
// two policies.
std::optional<int>
openDirectoryChainNoFollow(const QString &trustedAnchorPath,
                           const QStringList &ownedSuffixComponents,
                           bool allowCreateMissingComponents = true) {
  const QByteArray anchorUtf8 = QFile::encodeName(trustedAnchorPath);
  int currentFd = ::open(anchorUtf8.constData(),
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (currentFd < 0) {
    return std::nullopt;
  }
  const MountIdentity anchorMount = mountIdentityForFd(currentFd);
  for (const QString &component : ownedSuffixComponents) {
    const QByteArray componentUtf8 = component.toUtf8();
    struct stat st {};
    if (fstatat(currentFd, componentUtf8.constData(), &st,
                AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno != ENOENT || !allowCreateMissingComponents) {
        ::close(currentFd);
        return std::nullopt;
      }
      // Missing: create it now, relative to the already-open, already
      // no-follow-verified parent -- never via a path string, so there
      // is no ancestor path resolution step an attacker's symlink could
      // ever be substituted into.
      if (mkdirat(currentFd, componentUtf8.constData(), 0700) != 0 &&
          errno != EEXIST) {
        ::close(currentFd);
        return std::nullopt;
      }
      if (fstatat(currentFd, componentUtf8.constData(), &st,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        ::close(currentFd);
        return std::nullopt;
      }
    }
    if (!S_ISDIR(st.st_mode)) {
      // Exists but is a symlink node, a regular file, or some other
      // non-directory object -- AT_SYMLINK_NOFOLLOW's stat reports
      // S_ISLNK for a symlink, never S_ISDIR, so this also rejects the
      // exact "component pre-planted as a symlink" attack.
      ::close(currentFd);
      return std::nullopt;
    }
    const int nextFd =
        openDirectoryComponentNoFollow(currentFd, componentUtf8.constData());
    if (nextFd < 0) {
      ::close(currentFd);
      return std::nullopt;
    }
    if (!mountIdentityMatches(mountIdentityForFd(nextFd), anchorMount)) {
      qWarning() << "AssetCache: refusing owned-suffix component" << component
                 << "-- resolves onto a different mount than its trusted "
                    "anchor (bind-mount escape guard during directory-"
                    "chain creation)";
      ::close(currentFd);
      ::close(nextFd);
      return std::nullopt;
    }
    ::close(currentFd);
    currentFd = nextFd;
  }
  return currentFd;
}

// Round-9+ review (HIGH, repeated across multiple prior rounds without
// full resolution): resolves `absoluteTargetPathIn` -- either the OS-
// provided default cache location plus this application's own
// "assets/v1" suffix, or an ENTIRE caller-supplied Config::directory --
// walking EVERY path component between a genuinely trusted starting
// point and the leaf, never trusting a multi-segment path string as a
// single opaque unit beyond that starting point regardless of whether
// the full path happens to already exist on disk.
//
// This directly closes the "precreated leaf behind an intermediate
// symlink" attack the review demonstrates: previously, a configured
// directory whose full path ALREADY existed (even by way of a
// symlinked ancestor an attacker pre-planted) was opened with a single
// ::open(path, O_NOFOLLOW) call -- and POSIX's O_NOFOLLOW only refuses
// a symlink AT THE FINAL path component; every intermediate component
// is resolved by the kernel exactly as an ordinary, unhardened open()
// would. A "longest already-existing prefix" shortcut cannot close this
// either: in the demonstrated attack the WHOLE path already exists
// (through the symlink), so nothing would ever be identified as
// "still needing a no-follow walk" at all.
//
// The starting point used here is the process's own user home
// directory (QDir::homePath()), NOT the filesystem root: walking
// no-follow from home never encounters the legitimate, OS-bootstrap-
// time symlinks that caused a real regression in an EARLIER version of
// this very fix that instead walked no-follow from "/" (see
// openDirectoryChainNoFollow()'s own comment for that history) -- e.g.
// macOS's classic `/var` -> `/private/var` (a real ancestor of this
// project's own QTemporaryDir-based test fixtures on that platform) or
// a Linux distribution's /usr-merge symlinks (`/bin` -> `/usr/bin` and
// similar) -- none of which are ordinarily ancestors of a normal user's
// own home directory, while a locally-writable, attacker-plantable
// symlink for ANY ancestor of a path under home is exactly the class of
// attack this hardening exists to catch. Mount-identity continuity
// (openDirectoryChainNoFollow()'s existing mountIdentityMatches()
// check, further strengthened by openat2's RESOLVE_NO_XDEV on Linux --
// see openDirectoryComponentNoFollow()'s comment) is likewise anchored
// to home itself, never to "/": a real multi-mount system very commonly
// has "/home" itself on a dedicated partition separate from "/", but
// everything BENEATH one user's own home directory is, for any normal
// single-disk installation this project targets (including a Steam
// Deck's own storage layout), on that SAME mount throughout -- so this
// check remains strict without rejecting ordinary, non-hostile setups.
//
// When `absoluteTargetPathIn` is NOT itself a descendant of home (an
// unusual, explicit system-wide configuration choice -- e.g. an admin
// pointing this cache at a shared "/srv/..." location), this instead
// falls back to the pre-existing "longest existing prefix is a single
// trusted anchor, only the not-yet-existing remainder is walked
// no-follow" policy: a narrower guarantee for that uncommon case, but
// not a regression from the behaviour that shipped before this fix, and
// clearly narrower-scope rather than silently unsafe.
std::optional<int>
resolveTrustedDirectoryNoFollow(const QString &absoluteTargetPathIn,
                                bool allowCreateMissingComponents) {
  const QString absoluteTargetPath = QDir::cleanPath(absoluteTargetPathIn);
  if (absoluteTargetPath.isEmpty() ||
      !absoluteTargetPath.startsWith(QLatin1Char('/'))) {
    return std::nullopt;
  }
  const QString home = QDir::cleanPath(QDir::homePath());
  if (!home.isEmpty() &&
      (absoluteTargetPath == home ||
       absoluteTargetPath.startsWith(home + QLatin1Char('/')))) {
    const QString relativeToHome =
        absoluteTargetPath == home ? QString()
                                   : absoluteTargetPath.mid(home.length() + 1);
    const QStringList components =
        relativeToHome.isEmpty()
            ? QStringList{}
            : relativeToHome.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return openDirectoryChainNoFollow(home, components,
                                      allowCreateMissingComponents);
  }
  // Fallback for a target outside home: find the longest EXISTING
  // prefix (by simple, symlink-FOLLOWING existence -- matching exactly
  // what a single opaque trusted-anchor open would have accepted
  // before this fix, so this is not a narrower guarantee than what
  // shipped previously for this uncommon case) and walk only whatever
  // remains beneath it no-follow, exactly like the home-anchored case
  // above.
  QString existingPrefix = absoluteTargetPath;
  QStringList missingSuffix;
  while (existingPrefix != QLatin1String("/") &&
         !QFileInfo::exists(existingPrefix)) {
    const int idx = existingPrefix.lastIndexOf(QLatin1Char('/'));
    const QString lastComponent = existingPrefix.mid(idx + 1);
    if (lastComponent.isEmpty()) {
      break;
    }
    missingSuffix.prepend(lastComponent);
    existingPrefix = idx <= 0 ? QStringLiteral("/") : existingPrefix.left(idx);
  }
  return openDirectoryChainNoFollow(existingPrefix, missingSuffix,
                                    allowCreateMissingComponents);
}
#endif

#if defined(Q_OS_UNIX)
// Descriptor-relative, no-follow recursive delete (review item 7): every
// decision about whether an entry is a directory to recurse into, a file
// to unlink, or a symlink node to unlink-but-never-follow is made via
// fstatat(..., AT_SYMLINK_NOFOLLOW) -- an lstat-equivalent that never
// resolves the final path component -- and every actual filesystem
// mutation (openat/unlinkat) is relative to an already-open directory
// file descriptor rather than a re-resolved path string. Critically,
// descending into a subdirectory uses openat(..., O_NOFOLLOW): if the
// entry was replaced by a symlink between the fstatat() check above and
// this open (a rename/replace TOCTOU race), the open itself fails
// (ELOOP) rather than silently following the attacker-controlled
// indirection -- there is no window in which a path string is
// re-resolved from the filesystem root.
//
// A symlink node encountered anywhere in the tree is unlinked (removed)
// itself; its target, wherever it points, is NEVER opened, stat'd
// through, or touched in any way. A hard-linked regular file is just an
// ordinary directory entry here: unlinkat() drops only that one link,
// never recursing into or otherwise treating a hardlink specially.
bool safeRemoveEntryAt(int parentFd, const char *name,
                       const MountIdentity &expectedMount);

bool safeRemoveDirectoryContentsAt(int dirFd,
                                   const MountIdentity &expectedMount) {
  DIR *dirStream = fdopendir(dirFd);
  if (!dirStream) {
    ::close(dirFd);
    return false;
  }
  bool allOk = true;
  errno = 0;
  while (struct dirent *entry = readdir(dirStream)) {
    if (qstrcmp(entry->d_name, ".") == 0 || qstrcmp(entry->d_name, "..") == 0) {
      continue;
    }
    if (!safeRemoveEntryAt(dirFd, entry->d_name, expectedMount)) {
      allOk = false;
    }
    errno = 0;
  }
  closedir(dirStream); // also closes dirFd
  return allOk;
}

bool safeRemoveEntryAt(int parentFd, const char *name,
                       const MountIdentity &expectedMount) {
  struct stat st {};
  if (fstatat(parentFd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
    return errno == ENOENT; // already gone: nothing left to do
  }
  if (S_ISDIR(st.st_mode)) {
    // Round-4/5 review item 9 (and round-6 item 5's mount-id
    // strengthening): refuse to descend into (let alone delete) a
    // subdirectory that resolves onto a DIFFERENT mount than the cache
    // root -- a stray bind mount planted under this cache's
    // exclusively-owned directory must never be recursed into or
    // unlinked; its contents live on a filesystem this cache does not
    // own at all. This is checked from the PARENT'S stat (both before
    // and, again, after actually opening it below), never assumed from
    // the listing alone, and now also compares the kernel mount id when
    // available -- not just st_dev -- so a same-device bind mount is
    // caught too (see MountIdentity's comment).
    if (!mountIdentityMatches(mountIdentityRelative(parentFd, name, st),
                              expectedMount)) {
      qWarning() << "AssetCache: refusing to descend into" << name
                 << "-- different mount than cache root (mount escape "
                    "guard)";
      return false;
    }
    const int childFd =
        openat(parentFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (childFd < 0) {
      // Either genuinely not a directory anymore (replaced by a
      // symlink between the fstatat() above and here) or some other
      // open failure -- either way, refuse to proceed rather than
      // guessing.
      return false;
    }
    // Re-verify the mount on the ACTUAL DESCRIPTOR just opened (never
    // trusting the fstatat() above alone): closes the TOCTOU window
    // where the entry could have been replaced by a different-mount
    // entry between that check and this open.
    struct stat openedSt {};
    if (::fstat(childFd, &openedSt) != 0 || !S_ISDIR(openedSt.st_mode) ||
        !mountIdentityMatches(mountIdentityForFd(childFd), expectedMount)) {
      ::close(childFd);
      return false;
    }
    if (!safeRemoveDirectoryContentsAt(childFd, expectedMount)) {
      return false;
    }
    return unlinkat(parentFd, name, AT_REMOVEDIR) == 0 || errno == ENOENT;
  }
  // A regular file, or a symlink node itself (S_ISLNK): unlinkat()
  // without AT_REMOVEDIR removes the directory ENTRY, never resolving or
  // following it even when it names a symlink. A regular file cannot
  // itself BE a different mount (only a directory can be a mount
  // point), so no mount check applies here.
  return unlinkat(parentFd, name, 0) == 0 || errno == ENOENT;
}
#endif

// Removes `name` (expected to be a directory relative to `parentFd`)
// and everything under it, using the no-follow, mount-checked
// descriptor-relative primitives above. Refuses outright (a safe no-op,
// returning false) if `name` is a symlink, is not a directory, does not
// exist, or resolves onto a different mount than `expectedMount` -- see
// safeRemoveEntryAt()'s comment for the full rationale. On a
// hypothetical platform with no such primitives available, this is a
// safe no-op rather than risking a follow-through-symlink recursive
// delete.
bool safeRemoveTreeRelative(int parentFd, const QString &name,
                            const MountIdentity &expectedMount) {
#if defined(Q_OS_UNIX)
  const QByteArray nameUtf8 = name.toUtf8();
  const int fd = openat(parentFd, nameUtf8.constData(),
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) ||
      !mountIdentityMatches(mountIdentityForFd(fd), expectedMount)) {
    ::close(fd);
    return false;
  }
  const bool contentsOk = safeRemoveDirectoryContentsAt(fd, expectedMount);
  const bool rmOk =
      unlinkat(parentFd, nameUtf8.constData(), AT_REMOVEDIR) == 0 ||
      errno == ENOENT;
  return contentsOk && rmOk;
#else
  Q_UNUSED(parentFd);
  Q_UNUSED(name);
  Q_UNUSED(expectedMount);
  return false;
#endif
}

// (safeRemoveTree(path) previously lived here, opening `path` directly by
// absolute string. Round-4/5 review item 3/9 replaces it entirely with
// safeRemoveTreeRelative() above -- descriptor-relative and
// device-checked -- so every caller (reapAndEnforceQuota()'s stray-
// directory cleanup) resolves a subdirectory only relative to the
// already-anchored `m_rootFd`, never by re-deriving a path string.)

} // namespace

AssetCache::AssetCache(Config config)
    : m_config(std::move(config)),
      m_memory(new QCache<QString, CachedEntry>()) {
  // Round-9+ review (MEDIUM): validated FIRST, before anything else --
  // including the directory resolution immediately below -- so an
  // invalid disk-quota limit can never reach reapAndEnforceQuota()'s
  // eviction math at all. See configHasValidDiskByteLimit()'s comment.
  if (!configHasValidDiskByteLimit(m_config)) {
    m_diskCacheDisabled = true;
  }
  m_memory->setMaxCost(configHasValidMemoryByteLimit(m_config)
                           ? m_config.memoryMaxCostBytes
                           : 0);
  // Round-9+ review (HIGH): the target directory this instance will
  // use is fixed here, BEFORE anything else touches the filesystem, but
  // is no longer split into an opaque "trusted anchor" (opened as a
  // single unit, its intermediate components never individually
  // examined) plus a small fixed-length "owned suffix" -- see
  // resolveTrustedDirectoryNoFollow()'s comment for why that split, as
  // it stood before this round, still let a caller-supplied
  // Config::directory's entire path (arbitrarily many components) be
  // trusted as a single unit via one leaf-only O_NOFOLLOW open, even
  // when an INTERMEDIATE ancestor had been replaced by an
  // attacker-planted symlink and the final leaf happened to already
  // exist (through that redirection). `allowCreateMissingComponents`
  // is true ONLY for the default-location case (this application's own
  // exclusively-owned "assets/v1" suffix beneath the OS-provided cache
  // base may be created fresh); a caller-supplied Config::directory
  // must already exist in full, component by component -- this
  // application still never creates any part of a caller-supplied
  // custom cache directory.
  const bool usingDefaultLocation = m_config.directory.isEmpty();
  if (usingDefaultLocation) {
    m_directory =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
        QStringLiteral("/assets/v1");
  } else {
    m_directory = m_config.directory;
  }
  // Review item 7: if the configured cache directory ALREADY exists as a
  // symlink, refuse to use it at all -- never follow it (which would
  // silently start writing at whatever it points to), and never let any
  // later store()/lookupDisk()/reap call touch it either.
  // QFileInfo::isSymLink() (unlike QDir::exists(), which follows) reports
  // the entry's own type without resolving it. The memory cache still
  // works normally -- only disk persistence is disabled for this
  // instance's entire lifetime. This is a cheap, redundant-by-design
  // fast path: the fd-based resolution below independently re-detects
  // the exact same condition (and every other symlinked-ancestor
  // variant this single leaf-only check cannot see), so this early
  // check changes no observable behaviour, it just avoids doing any
  // filesystem-descriptor work at all for the single most common
  // misconfiguration case.
  if (QFileInfo(m_directory).isSymLink()) {
    m_diskCacheDisabled = true;
  }
#if defined(Q_OS_UNIX)
  // Review round-3 item 9: open and retain a directory descriptor for
  // `m_directory` NOW, at construction, and record the (device, inode)
  // pair it names via fstat() on THIS SAME descriptor -- never a second,
  // later path-based stat, which could observe a DIFFERENT filesystem
  // object if the path has since been replaced. Every later
  // disk-touching operation re-derives the CURRENT (device, inode) for
  // this same path and compares it against these retained values
  // (verifyRootAnchorLocked()) before proceeding, so a root directory
  // renamed away and replaced by a new one (or an ancestor component
  // replaced such that the same path now resolves elsewhere) is
  // detected and disk I/O is permanently disabled rather than silently
  // operating against a different object than the one this instance
  // was constructed against.
  if (!m_diskCacheDisabled) {
    // Round-9+ review: resolved by walking EVERY path component from a
    // genuinely trusted starting point (see
    // resolveTrustedDirectoryNoFollow()'s comment) rather than trusting
    // an entire multi-segment configured path as one opaque unit.
    const std::optional<int> chainFd =
        resolveTrustedDirectoryNoFollow(m_directory, usingDefaultLocation);
    struct stat st {};
    if (!chainFd || ::fstat(*chainFd, &st) != 0) {
      if (chainFd) {
        ::close(*chainFd);
      }
      m_diskCacheDisabled = true;
    } else {
      m_rootFd = *chainFd;
      m_rootDevice = static_cast<quint64>(st.st_dev);
      m_rootInode = static_cast<quint64>(st.st_ino);
      const MountIdentity rootMount = mountIdentityForFd(m_rootFd);
      m_rootMountId = rootMount.mountId;
      m_rootHasMountId = rootMount.hasMountId;
    }
  }
#endif
  reapAndEnforceQuota();
}

AssetCache::~AssetCache() {
  delete m_memory;
#if defined(Q_OS_UNIX)
  if (m_rootFd >= 0) {
    ::close(m_rootFd);
  }
#endif
}

QString AssetCache::cacheKeyFor(const QUrl &resolvedCandidateUrl) {
  const QByteArray canonical =
      ("assetcache-v1\n" + resolvedCandidateUrl.toString(QUrl::FullyEncoded))
          .toUtf8();
  return QString::fromLatin1(
      QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

QString AssetCache::manifestPath(const QString &key) const {
  return key + ".manifest.json"_L1;
}

QString AssetCache::generationPayloadPath(const QString &key,
                                          const QString &generation) const {
  return key + u'.' + generation + ".bin"_L1;
}

QString AssetCache::generationMetadataPath(const QString &key,
                                           const QString &generation) const {
  return key + u'.' + generation + ".meta.json"_L1;
}

QString AssetCache::manifestPathForTesting(const QString &directory,
                                           const QString &key) {
  return directory + u'/' + key + ".manifest.json"_L1;
}

QString AssetCache::payloadPathForTesting(const QString &directory,
                                          const QString &key,
                                          const QString &generation) {
  return directory + u'/' + key + u'.' + generation + ".bin"_L1;
}

QString AssetCache::metadataPathForTesting(const QString &directory,
                                           const QString &key,
                                           const QString &generation) {
  return directory + u'/' + key + u'.' + generation + ".meta.json"_L1;
}

std::optional<AssetCache::CachedEntry>
AssetCache::lookupMemory(const QString &key) {
  QMutexLocker locker(&m_mutex);
  CachedEntry *entry = m_memory->object(key);
  if (!entry) {
    return std::nullopt;
  }
  const CachedEntry hit = *entry;
  // Review item 11: a memory hit is a genuine access and must be
  // reflected in this key's PERSISTED disk recency, not just its
  // in-memory presence -- see touchAccessRecencyLocked()'s comment and
  // the class comment for the full rationale.
  touchAccessRecencyLocked(key);
  return hit;
}

quint64 AssetCache::nextAccessSequenceLocked() {
  return m_nextAccessSequence++;
}

void AssetCache::touchAccessRecencyLocked(const QString &key) {
  if (m_diskCacheDisabled || !isValidKey(key)) {
    return;
  }
  // Round-4/5 review item 3: a MEMORY hit (lookupMemory() -> here) must
  // be just as anchor-verified as a real disk read before this method
  // touches the filesystem at all -- previously this call was missing
  // entirely on the memory-hit path, letting a memory-hit recency bump
  // proceed even after the root had been replaced post-construction.
  if (!verifyRootAnchorLocked()) {
    return;
  }
  const std::optional<QString> generation = readManifestGeneration(key);
  if (!generation) {
    return; // nothing on disk for this key right now -- nothing to bump
  }
  const QString metadataName = generationMetadataPath(key, *generation);
  const std::optional<DiskMetadata> metadata = readMetadata(metadataName, key);
  if (!metadata || metadata->generationId != *generation) {
    // A corrupt/self-inconsistent record here is a real-repair signal,
    // but repairing it is lookupDisk()/reapAndEnforceQuota()'s job (both
    // independently re-verify the payload itself before trusting
    // anything) -- this purely-cosmetic recency bump must not delete a
    // record it hasn't actually read/verified the PAYLOAD of.
    return;
  }
  DiskMetadata refreshed = *metadata;
  refreshed.lastAccessMsecsSinceEpoch = QDateTime::currentMSecsSinceEpoch();
  refreshed.accessSequence = nextAccessSequenceLocked();
  // Review item 11: `durable = false` -- see writeMetadata()'s
  // declaration comment and the class comment for why a lost recency
  // bump on crash only ever affects eviction ordering, never integrity.
  (void)writeMetadata(metadataName, refreshed, /*durable=*/false);
}

bool AssetCache::verifyRootAnchorLocked() const {
  if (m_diskCacheDisabled) {
    return false;
  }
#if defined(Q_OS_UNIX)
  if (m_rootFd < 0) {
    // Never opened successfully at construction (or on a platform
    // without this protection) -- treat as already disabled rather than
    // silently skipping the check.
    m_diskCacheDisabled = true;
    return false;
  }
  struct stat currentSt {};
  // lstat (never a path resolved THROUGH the retained descriptor, and
  // never following a final symlink component): re-derives what
  // `m_directory` names RIGHT NOW, independent of the object m_rootFd
  // still refers to. If the path has been renamed away, removed, or
  // replaced by anything else (a plain directory, a symlink, a
  // dangling entry), the (device, inode) pair observed here will not
  // match what was captured from m_rootFd at construction. Round-4/5
  // review item 3: this check is now purely ADVISORY (it stops future
  // writes into a directory this instance no longer semantically
  // considers its own the moment it can detect that) -- every actual
  // disk operation's real safety comes from resolving through
  // `m_rootFd` directly (openat/fstatat/renameat/unlinkat), which
  // remains anchored to the ORIGINAL filesystem object regardless of
  // what the path currently names, so a race between this check and a
  // later operation can no longer let that operation touch the wrong
  // object the way a path-reopening implementation could.
  const QByteArray dirUtf8 = QFile::encodeName(m_directory);
  if (::lstat(dirUtf8.constData(), &currentSt) != 0 ||
      static_cast<quint64>(currentSt.st_dev) != m_rootDevice ||
      static_cast<quint64>(currentSt.st_ino) != m_rootInode) {
    qWarning() << "AssetCache: root directory anchor mismatch for"
               << m_directory
               << "-- permanently disabling disk I/O for this instance";
    m_diskCacheDisabled = true;
    ::close(m_rootFd);
    m_rootFd = -1;
    return false;
  }
  return true;
#else
  return true;
#endif
}

bool AssetCache::fsyncRootLocked() const {
#if defined(Q_OS_UNIX)
  return m_rootFd >= 0 && ::fsync(m_rootFd) == 0;
#else
  return true;
#endif
}

QString AssetCache::mintGenerationIdLocked(quint64 accessSequence) {
  // Round-4/5 review item 4: independent of the payload's own content
  // hash -- see DiskMetadata::generationId's comment for why a
  // content-addressed generation identifier allows a same-bytes
  // replacement to rewrite an already-live file in place. `accessSequence`
  // (unique and monotonic for this store() transaction) plus real OS
  // entropy means two mints can never collide even if minted in the same
  // process tick; hashed down to the existing 64-lowercase-hex shape so
  // no on-disk filename-pattern change is required.
  quint64 entropy[2] = {0, 0};
  QRandomGenerator::system()->fillRange(entropy, 2);
  const QByteArray material =
      QByteArray("assetcache-generation-v1\n") +
      QByteArray::number(static_cast<qulonglong>(accessSequence)) + '\n' +
      QByteArray::number(static_cast<qulonglong>(entropy[0]), 16) + '\n' +
      QByteArray::number(static_cast<qulonglong>(entropy[1]), 16);
  return QString::fromLatin1(
      QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
}

#if defined(Q_OS_UNIX)
namespace {
// Recursive, descriptor-relative, no-follow, mount-bounded byte-usage
// walker (review round-4/5 items 3, 9, 11; round-6 item 5's mount-id
// strengthening): sums the on-disk size of every regular
// file/symlink-node entry found anywhere under `dirFd` (which must be
// open for exactly one call -- this function always closes it),
// recursing into subdirectories ONLY while they remain on
// `expectedMount`; a same-mount directory's OWN entry size is never
// counted (only the files found underneath it), matching this walker's
// historical QDir::Files-equivalent semantics. A subdirectory found to
// be a different mount (a stray bind mount planted under this cache's
// exclusively-owned directory -- including a SAME-DEVICE bind mount,
// which mountIdentityMatches() now also detects) is never descended
// into -- but its OWN directory-entry size IS added in that one case,
// so it can never simply vanish from the total the way silently
// skipping it would; quota enforcement must see it as real,
// non-reclaimable usage (see safeRemoveEntryAt()'s matching refusal to
// ever delete across that same boundary) rather than falsely reporting
// the cache as "under budget" while genuinely undeletable/unaccounted
// bytes remain resident.
qint64 sumUsageRelative(int dirFd, const MountIdentity &expectedMount) {
  qint64 total = 0;
  DIR *dirStream = fdopendir(dirFd);
  if (!dirStream) {
    ::close(dirFd);
    return 0;
  }
  errno = 0;
  while (struct dirent *entry = readdir(dirStream)) {
    if (qstrcmp(entry->d_name, ".") == 0 || qstrcmp(entry->d_name, "..") == 0) {
      errno = 0;
      continue;
    }
    struct stat st {};
    if (fstatat(dirFd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      errno = 0;
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      if (!mountIdentityMatches(mountIdentityRelative(dirFd, entry->d_name, st),
                                expectedMount)) {
        // Cross-mount entry encountered: this directory can never be
        // descended into or deleted (see safeRemoveEntryAt()'s matching
        // refusal), so its own directory-entry size is counted here as
        // an irreducible placeholder -- otherwise it would simply
        // vanish from the total, letting quota enforcement falsely
        // believe the cache is under budget while genuinely
        // undeletable/unaccounted bytes remain resident. A same-mount
        // directory is never counted this way (matching this walker's
        // historical QDir::Files-equivalent semantics): only the
        // regular files found underneath it contribute bytes.
        total += st.st_size;
        errno = 0;
        continue;
      }
      const int childFd =
          openat(dirFd, entry->d_name,
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (childFd >= 0) {
        total += sumUsageRelative(childFd, expectedMount); // closes childFd
      }
    } else {
      total += st.st_size;
    }
    errno = 0;
  }
  closedir(dirStream); // also closes dirFd
  return total;
}
} // namespace
#endif

qint64 AssetCache::diskUsageBytesLocked() const {
  if (!verifyRootAnchorLocked()) {
    return 0;
  }
  // See diskUsageBytes()'s comment (this is its lock-already-held
  // implementation, reused directly by reapAndEnforceQuota() -- review
  // round-3 item 11 -- so its eviction-target math is always driven by
  // a true, unconditional inventory rather than only the subset of
  // entries that happened to validate cleanly). Round-4/5 review item
  // 3: resolved via a FRESH directory handle on the already-anchored
  // `m_rootFd` (see openFreshHandleToSameDirectory()'s comment -- never
  // a bare dup(), which would share `m_rootFd`'s read offset with
  // whatever OTHER listing may have already been performed against it
  // this call, and never a QDirIterator re-resolving `m_directory` by
  // path).
#if defined(Q_OS_UNIX)
  const int freshFd = openFreshHandleToSameDirectory(m_rootFd);
  if (freshFd < 0) {
    return 0;
  }
  return sumUsageRelative(freshFd,
                          MountIdentity{m_rootDevice, m_rootMountId,
                                        m_rootHasMountId}); // closes freshFd
#else
  return 0;
#endif
}

bool AssetCache::writeMetadata(const QString &metadataFilePath,
                               const DiskMetadata &metadata,
                               bool durable) const {
  QJsonObject obj;
  obj[QStringLiteral("formatVersion")] = kMetadataFormatVersion;
  obj[QStringLiteral("key")] = metadata.key;
  obj[QStringLiteral("contentType")] = metadata.contentType;
  obj[QStringLiteral("encodedSize")] = metadata.encodedSize;
  obj[QStringLiteral("width")] = metadata.width;
  obj[QStringLiteral("height")] = metadata.height;
  obj[QStringLiteral("sha256")] = metadata.sha256Hex;
  // Round-4/5 review item 4: the generation identifier is persisted
  // explicitly and independently of `sha256Hex` -- see
  // DiskMetadata::generationId's comment.
  obj[QStringLiteral("generationId")] = metadata.generationId;
  obj[QStringLiteral("etag")] = metadata.etag;
  obj[QStringLiteral("lastModified")] = metadata.lastModified;
  obj[QStringLiteral("insertedAtMs")] = metadata.insertedAtMsecsSinceEpoch;
  obj[QStringLiteral("lastAccessMs")] = metadata.lastAccessMsecsSinceEpoch;
  // Review round-3 item 8: persisted as a decimal STRING, not a bare
  // JSON number -- see kMaxExactJsonIntegerDouble's comment. A JSON
  // number is an IEEE-754 double and can only represent every integer
  // *exactly* up to 2^53; this counter increments once per successful
  // access/store and, over a long enough cache lifetime, could
  // plausibly exceed that threshold, silently losing precision (and
  // therefore LRU ordering correctness) if stored as a number. A
  // quint64-range string has no such ceiling.
  obj[QStringLiteral("accessSeq")] = QString::number(metadata.accessSequence);

#if defined(Q_OS_UNIX)
  // Round-4/5 review item 3: fd-relative atomic write -- see
  // writeFileAtomicRelative()'s comment. `durable` is false only for a
  // pure recency-only bump with no other semantic change (see this
  // method's declaration comment for why skipping the fsync there is
  // safe).
  return m_rootFd >= 0 &&
         writeFileAtomicRelative(
             m_rootFd, metadataFilePath,
             QJsonDocument(obj).toJson(QJsonDocument::Compact), durable);
#else
  Q_UNUSED(metadataFilePath);
  Q_UNUSED(durable);
  return false;
#endif
}

bool AssetCache::writeManifest(const QString &key,
                               const QString &generation) const {
  QJsonObject obj;
  obj[QStringLiteral("formatVersion")] = kMetadataFormatVersion;
  obj[QStringLiteral("key")] = key;
  obj[QStringLiteral("generation")] = generation;

#if defined(Q_OS_UNIX)
  return m_rootFd >= 0 && writeFileAtomicRelative(
                              m_rootFd, manifestPath(key),
                              QJsonDocument(obj).toJson(QJsonDocument::Compact),
                              /*durable=*/true);
#else
  Q_UNUSED(key);
  Q_UNUSED(generation);
  return false;
#endif
}

std::optional<QString>
AssetCache::readManifestGeneration(const QString &key) const {
#if defined(Q_OS_UNIX)
  if (m_rootFd < 0) {
    return std::nullopt;
  }
  const std::optional<QByteArray> raw = readBoundedRelative(
      m_rootFd, MountIdentity{m_rootDevice, m_rootMountId, m_rootHasMountId},
      manifestPath(key), kMaxMetadataBytesOnDisk);
  if (!raw) {
    return std::nullopt;
  }
  const QJsonDocument doc = QJsonDocument::fromJson(*raw);
  if (!doc.isObject()) {
    return std::nullopt;
  }
  const QJsonObject obj = doc.object();
  if (obj[QStringLiteral("formatVersion")].toInt(-1) !=
          kMetadataFormatVersion ||
      obj[QStringLiteral("key")].toString() != key) {
    return std::nullopt;
  }
  const QString generation = obj[QStringLiteral("generation")].toString();
  // A generation identifier is always a syntactically 64-hex string --
  // exactly the same shape as a cache key -- and this string becomes
  // part of a filesystem path below (generationPayloadPath()/
  // generationMetadataPath()): never trust it otherwise.
  if (!validKeyPattern().match(generation).hasMatch()) {
    return std::nullopt;
  }
  return generation;
#else
  Q_UNUSED(key);
  return std::nullopt;
#endif
}

std::optional<AssetCache::DiskMetadata>
AssetCache::readMetadata(const QString &metadataFilePath,
                         const QString &expectedKey) const {
#if defined(Q_OS_UNIX)
  if (m_rootFd < 0) {
    return std::nullopt;
  }
  // Round-4/5 review item 3: fd-relative bounded read -- see
  // readBoundedRelative()'s comment. Bounding the read itself (not just
  // a preceding stat) means this can never allocate more than
  // kMaxMetadataBytesOnDisk + 1 bytes regardless of how large a
  // corrupted/locally-planted file has actually become by the time this
  // read runs.
  const std::optional<QByteArray> rawOpt = readBoundedRelative(
      m_rootFd, MountIdentity{m_rootDevice, m_rootMountId, m_rootHasMountId},
      metadataFilePath, kMaxMetadataBytesOnDisk);
  if (!rawOpt) {
    return std::nullopt;
  }
  const QByteArray &raw = *rawOpt;
#else
  Q_UNUSED(metadataFilePath);
  Q_UNUSED(expectedKey);
  return std::nullopt;
#endif
#if defined(Q_OS_UNIX)
  const QJsonDocument doc = QJsonDocument::fromJson(raw);
  if (!doc.isObject()) {
    return std::nullopt;
  }
  const QJsonObject obj = doc.object();
  if (obj[QStringLiteral("formatVersion")].toInt(-1) !=
      kMetadataFormatVersion) {
    return std::nullopt;
  }
  DiskMetadata metadata;
  metadata.key = obj[QStringLiteral("key")].toString();
  metadata.contentType = obj[QStringLiteral("contentType")].toString();
  // Review round-3 item 8: every numeric field below is independently
  // validated as finite/integral/non-negative/in-range BEFORE any cast
  // to a narrower type -- a corrupted or maliciously-planted metadata
  // file can otherwise supply a fractional, negative, NaN, +/-infinity,
  // or absurdly oversized JSON number, and casting THAT directly to
  // qint64/int/quint64 (as this file used to do) is undefined behaviour,
  // not merely "produces a wrong value". Any field that fails this check
  // renders the whole metadata record untrusted (return std::nullopt
  // below), which the caller then treats as a quarantine-and-refetch
  // candidate exactly like a missing/absent file.
  const std::optional<qint64> encodedSize = readBoundedNonNegativeIntegerField(
      obj[QStringLiteral("encodedSize")], kMaxSinglePayloadBytesOnDisk);
  const std::optional<qint64> width = readBoundedNonNegativeIntegerField(
      obj[QStringLiteral("width")], kMaxDimensionPixelsOnDisk);
  const std::optional<qint64> height = readBoundedNonNegativeIntegerField(
      obj[QStringLiteral("height")], kMaxDimensionPixelsOnDisk);
  const std::optional<qint64> insertedAtMs = readBoundedNonNegativeIntegerField(
      obj[QStringLiteral("insertedAtMs")], kMaxExactJsonIntegerDouble);
  const std::optional<qint64> lastAccessMs = readBoundedNonNegativeIntegerField(
      obj[QStringLiteral("lastAccessMs")], kMaxExactJsonIntegerDouble);
  if (!encodedSize || !width || !height || !insertedAtMs || !lastAccessMs) {
    return std::nullopt;
  }
  metadata.encodedSize = *encodedSize;
  metadata.width = static_cast<int>(*width);
  metadata.height = static_cast<int>(*height);
  metadata.sha256Hex = obj[QStringLiteral("sha256")].toString();
  // Round-4/5 review item 4: the explicit generation-identity witness --
  // see DiskMetadata::generationId's comment. Absent (a pre-this-fix
  // metadata file) parses as an empty string, which never matches a
  // real (64-hex) generation identifier, so such a legacy record is
  // treated as untrusted/self-inconsistent by every caller that compares
  // it against the manifest's generation -- exactly the quarantine
  // behaviour a genuinely corrupt record already receives.
  metadata.generationId = obj[QStringLiteral("generationId")].toString();
  metadata.etag = obj[QStringLiteral("etag")].toString();
  metadata.lastModified = obj[QStringLiteral("lastModified")].toString();
  metadata.insertedAtMsecsSinceEpoch = *insertedAtMs;
  metadata.lastAccessMsecsSinceEpoch = *lastAccessMs;
  // Review round-3 item 8: accessSeq is persisted as a decimal STRING
  // (see writeMetadata()'s comment), not a bare JSON number -- parse it
  // with QString::toULongLong()'s own strict validation (rejects
  // fractional/non-digit/out-of-quint64-range content via `ok`) rather
  // than a JSON-number cast at all. Absent/malformed defaults to 0
  // (the same default a pre-existing/legacy metadata file with no such
  // field at all would have produced), never an arbitrary or partially-
  // parsed value.
  bool accessSeqOk = false;
  const quint64 accessSeq =
      obj[QStringLiteral("accessSeq")].toString().toULongLong(&accessSeqOk);
  metadata.accessSequence = accessSeqOk ? accessSeq : 0;
  if (metadata.key != expectedKey || metadata.sha256Hex.isEmpty() ||
      metadata.encodedSize < 0) {
    return std::nullopt;
  }
  return metadata;
#endif
}

AssetCache::DeleteEntryOutcome
AssetCache::deleteEntry(const QString &key) const {
  if (m_diskCacheDisabled) {
    // Disk I/O has been disabled for this instance's ENTIRE lifetime
    // (a symlink detected at construction, an invalid Config byte
    // limit, or an EARLIER call that already latched this via
    // verifyRootAnchorLocked() below) -- there never was, and never
    // will be, any disk for THIS instance to persist an entry to, so
    // "everything reclaimed" / "manifest durably absent" really are
    // vacuously true for every observable purpose this instance itself
    // cares about.
    return {true, true};
  }
  if (!verifyRootAnchorLocked()) {
    // Round-9+ review (HIGH): root verification failing HERE, on THIS
    // specific call (as opposed to disk having already been disabled
    // before this call even started -- the case handled above), is
    // fundamentally different and must NOT be folded into the same
    // vacuous {true, true} result the previous implementation returned
    // for both. A real, still-live on-disk manifest for `key` may
    // genuinely exist right now, completely untouched -- this call
    // never got as far as even attempting to remove it. Reporting
    // {true, true} here would let invalidate() (and, through it, an
    // authoritative negative-404 record) believe this key's entry is
    // now durably, crash-confirmed gone -- when in fact a DIFFERENT
    // AssetCache instance (a fresh process started later, or a sibling
    // instance pointed at the same directory) that does not share this
    // exact transient/verification failure could still open that very
    // manifest and let the stale entry "revive" once this process's own
    // negative-404 TTL, or its own process lifetime, has ended. Neither
    // "reclaimed" nor "durably absent" is a safe promise to make, so
    // both are false: the caller (invalidate()) surfaces this as its own
    // typed PersistenceFailed result instead of silently claiming
    // success.
    return {false, false};
  }
  // Review item 8: `key` no longer maps to a fixed pair of filenames --
  // reclaim EVERY file this cache could ever have written for it (the
  // manifest, plus every generation-scoped payload/metadata file,
  // whether it's the live generation or an orphan left by an
  // interrupted replacement) via a name-prefix sweep, rather than
  // guessing at a single generation. `key` is always validated
  // (isValidKey()) by every public entry point before reaching here, so
  // it can never itself contain a wildcard-special character. Round-4/5
  // review item 3: both the listing and every removal are resolved
  // relative to `m_rootFd`, never a QDir re-resolving `m_directory` by
  // path.
#if defined(Q_OS_UNIX)
  if (m_rootFd < 0) {
    return {true, true};
  }
  const QString manifestName = manifestPath(key);

  // Round-6 item 6 / round-7/8 item 6: the manifest is the ONE file
  // whose presence/absence actually decides whether a future lookup can
  // ever find this key again -- every other matched file is only
  // reachable BY WAY OF the manifest naming its generation (see
  // readManifestGeneration()). Unlink it UNCONDITIONALLY -- never gated
  // on whether a (possibly-failed) prefix enumeration happened to list
  // it first -- then fsync the root directory so that unlink is durable
  // before this function does anything else. removeFileRelative()
  // already treats an already-absent file (ENOENT) as success, so this
  // is exactly as safe to call when the manifest never existed as it is
  // when it does; the prior "only unlink if the listing found it" gate
  // meant a transient enumeration failure (a temporarily exhausted fd
  // table, a directory-stream allocation failure, etc.) silently
  // skipped this unlink entirely while still reporting the manifest as
  // durably gone, letting the OLD entry revive after the process
  // restarts or the negative-cache TTL for a definitive 404 expires.
  const bool manifestRemoveOk = removeFileRelative(m_rootFd, manifestName);
  const bool manifestFsyncOk = manifestRemoveOk && fsyncRootLocked();
  const bool manifestDurablyAbsent = manifestRemoveOk && manifestFsyncOk;

  // Round-7/8 item 6: an unavailable directory listing is NOT the same
  // as "durably nothing left to reclaim" -- if the prefix enumeration
  // itself could not be completed, this key's remaining on-disk
  // footprint (any orphaned generation payload/metadata file) cannot be
  // confirmed reclaimed at all, so this must report `allFilesReclaimed
  // = false` rather than vacuously `true`. The manifest's own durable
  // removal above is entirely unaffected either way: it never depended
  // on this listing succeeding in the first place.
  const std::optional<QStringList> matches =
      listNamesWithPrefixRelative(m_rootFd, key + u'.');
  if (!matches) {
    return {false, manifestDurablyAbsent};
  }

  // Review item 11: report whether EVERY matched file was actually
  // removed. A failed unlink (e.g. a permission error, or -- in tests --
  // a directory planted at the same path) means this key's disk
  // footprint was NOT fully reclaimed; a caller doing quota accounting
  // must not credit itself with bytes that are still genuinely occupied.
  bool allRemoved = manifestRemoveOk;
  for (const QString &name : *matches) {
    if (name == manifestName) {
      continue; // already handled, durably, above
    }
    if (!removeFileRelative(m_rootFd, name)) {
      allRemoved = false;
    }
  }
  return {allRemoved, manifestDurablyAbsent};
#else
  return {true, true};
#endif
}

std::optional<AssetCache::CachedEntry>
AssetCache::lookupDisk(const QString &key) {
  // Check memory first: promote-on-disk-hit still applies, but there is no
  // reason to touch the filesystem at all if the entry is already resident.
  if (auto memoryHit = lookupMemory(key)) {
    return memoryHit;
  }

  if (m_diskCacheDisabled) {
    // Review item 7: the configured root was itself a symlink at
    // construction time -- never read through it.
    return std::nullopt;
  }

  if (!isValidKey(key)) {
    // Never let a malformed key (path separators, "..", etc.) reach
    // manifestPath()/generationPayloadPath()/generationMetadataPath()
    // below -- see isValidKey()'s comment.
    return std::nullopt;
  }

  QMutexLocker locker(&m_mutex);

  if (!verifyRootAnchorLocked()) {
    // Review round-3 item 9: the root has been replaced/removed/mounted
    // over since construction -- refuse to read through it at all.
    return std::nullopt;
  }

  const std::optional<QString> generation = readManifestGeneration(key);
  if (!generation) {
    // Manifest missing or corrupt: nothing for this key can be trusted
    // (see the class comment -- the manifest is the sole pointer to
    // which generation, if any, is live). Reclaim anything left behind
    // for this key -- including any orphaned generation files -- rather
    // than leaking it until the next sweep.
    (void)deleteEntry(key);
    return std::nullopt;
  }

  const std::optional<DiskMetadata> metadata =
      readMetadata(generationMetadataPath(key, *generation), key);
  if (!metadata || metadata->generationId != *generation) {
    // Metadata missing/corrupt, or (defense in depth) it does not even
    // claim to be the generation its own filename says it is.
    (void)deleteEntry(key);
    return std::nullopt;
  }

#if defined(Q_OS_UNIX)
  // Round-4/5 review item 3: fd-relative, size-verified read -- see
  // readExactSizeVerifiedRelative()'s comment. Rejected on size alone
  // before any content beyond the probe byte is trusted; never a
  // QFile reopening `m_directory` by path.
  const std::optional<QByteArray> verifiedBytes =
      m_rootFd >= 0
          ? readExactSizeVerifiedRelative(
                m_rootFd,
                MountIdentity{m_rootDevice, m_rootMountId, m_rootHasMountId},
                generationPayloadPath(key, *generation), metadata->encodedSize,
                kMaxSinglePayloadBytesOnDisk)
          : std::nullopt;
#else
  const std::optional<QByteArray> verifiedBytes = std::nullopt;
#endif
  if (!verifiedBytes) {
    // Manifest+metadata present but this generation's payload missing,
    // unreadable, or size-mismatched: corrupt/incomplete entry -- see
    // readExactSizeVerifiedRelative()'s comment. Never trust a payload
    // whose declared or actual size can't possibly be valid.
    (void)deleteEntry(key);
    return std::nullopt;
  }
  const QByteArray &bytes = *verifiedBytes;

  const QString actualSha256 = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  if (actualSha256 != metadata->sha256Hex ||
      bytes.size() != metadata->encodedSize) {
    // Payload does not match the metadata that vouches for it: never
    // trust a mismatched pair, no matter which file is "actually" wrong.
    (void)deleteEntry(key);
    return std::nullopt;
  }

  CachedEntry entry;
  entry.encodedBytes = bytes;
  entry.contentType = metadata->contentType;
  entry.dimensions = QSize(metadata->width, metadata->height);
  entry.sha256Hex = metadata->sha256Hex;
  entry.etag = metadata->etag;
  entry.lastModified = metadata->lastModified;
  entry.insertedAtMsecsSinceEpoch = metadata->insertedAtMsecsSinceEpoch;
  entry.lastAccessMsecsSinceEpoch = QDateTime::currentMSecsSinceEpoch();

  // Refresh on-disk lastAccess so LRU eviction reflects this real read,
  // per the class comment's metadata-driven-LRU policy (never filesystem
  // atime). This rewrites the SAME generation's metadata file in place
  // (the generation, and therefore the manifest, is unchanged) -- a
  // failure here only loses this one LRU-freshness update, never the
  // entry's validity: writeMetadata() is itself atomic
  // (writeFileAtomicRelative(), a temp-file-plus-renameat write), so a
  // crash mid-write leaves the prior, still-fully-valid metadata
  // untouched. Review item 11: also mints a fresh monotonic access
  // sequence (the primary eviction-ordering key -- see the class
  // comment), and this bump is deliberately non-durable
  // (`durable=false`): losing it to a crash only affects eviction
  // ordering, never this entry's integrity.
  DiskMetadata refreshed = *metadata;
  refreshed.lastAccessMsecsSinceEpoch = entry.lastAccessMsecsSinceEpoch;
  refreshed.accessSequence = nextAccessSequenceLocked();
  (void)writeMetadata(generationMetadataPath(key, *generation), refreshed,
                      /*durable=*/false);

  // Promote into memory ONLY when this entry needs no further
  // revalidation. AssetRequestCoordinator's memory-hit path (see
  // AssetRequestCoordinator.cpp) trusts a memory hit unconditionally, with
  // no conditional GET or etag/lastModified check at all -- so promoting
  // an entry that still carries a validator here would let it become a
  // trusted memory hit for every later lookupMemory() call BEFORE this
  // process has ever actually revalidated it against the origin (e.g. two
  // request() calls issued back-to-back, the second landing here while
  // the first's real conditional GET for the very same bytes is still in
  // flight). AssetRequestCoordinator only ever calls
  // AssetCache::store()/touchAfterNotModified() -- which manage the
  // memory entry directly -- once a real revalidation has actually
  // completed, so this is the only path that must withhold promotion.
  if (entry.etag.isEmpty() && entry.lastModified.isEmpty()) {
    auto *heapEntry = new CachedEntry(entry);
    m_memory->insert(key, heapEntry, static_cast<qsizetype>(entry.costBytes()));
  }

  return entry;
}

void AssetCache::store(const QString &key, CachedEntry entry) {
  if (!isValidKey(key)) {
    // Never let a malformed key reach payloadPath()/metadataPath() below
    // -- see isValidKey()'s comment. Rejecting the whole store() as a
    // no-op (rather than, say, still inserting into the memory cache
    // under an untrusted key) keeps the invariant simple: an entry only
    // ever exists, in memory or on disk, under a key this cache itself
    // considers well-formed.
    return;
  }

  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (entry.insertedAtMsecsSinceEpoch == 0) {
    entry.insertedAtMsecsSinceEpoch = now;
  }
  if (entry.lastAccessMsecsSinceEpoch == 0) {
    entry.lastAccessMsecsSinceEpoch = now;
  }
  // Review item 8: ALWAYS (re)compute this from the actual bytes being
  // stored, never trusting a caller-supplied value verbatim -- this
  // string becomes the generation's filename segment below
  // (generationPayloadPath()/generationMetadataPath()), so a malformed
  // or attacker-influenced caller-supplied sha256Hex must never reach a
  // filesystem path. Every legitimate caller already derives this value
  // from a hash of these exact bytes anyway, so recomputing changes
  // nothing for them.
  entry.sha256Hex = QString::fromLatin1(
      QCryptographicHash::hash(entry.encodedBytes, QCryptographicHash::Sha256)
          .toHex());

  {
    QMutexLocker locker(&m_mutex);
    auto *heapEntry = new CachedEntry(entry);
    m_memory->insert(key, heapEntry, static_cast<qsizetype>(entry.costBytes()));

    // Never write a payload to disk that this cache could never itself
    // read back: readExactSizeVerifiedRelative() hard-caps any disk
    // read at kMaxSinglePayloadBytesOnDisk, so a larger on-disk file
    // would sit as unreclaimable disk usage (never a valid lookupDisk()
    // hit) until a later reap sweep happens to notice it. Production
    // callers already cap fetched bytes well below this via
    // AssetNetworkFetcher::Limits::maxEncodedBytes before a fetch even
    // completes; this guard is defense-in-depth against a bug or a
    // future caller invoking store() directly with an oversized entry.
    // The memory insert above is unaffected -- QCache's own cost-based
    // eviction already bounds memory usage independently of disk.
    if (m_diskCacheDisabled || !verifyRootAnchorLocked()) {
      // Review item 7 / round-3 item 9: root was a symlink at
      // construction, or has since been replaced/removed/mounted over.
      // Memory insert above still happened -- only the disk write is
      // skipped.
    } else if (entry.encodedBytes.size() <= kMaxSinglePayloadBytesOnDisk) {
#if defined(Q_OS_UNIX)
      if (m_rootFd < 0) {
        // verifyRootAnchorLocked() above already guards this in
        // practice, but guard defensively against any future caller
        // path that reaches here without it.
      } else {
        // Round-4/5 review item 4: the generation identifier is now a
        // fresh, unique value minted for THIS store() transaction --
        // deliberately NOT the payload's own content hash (see
        // mintGenerationIdLocked()'s and DiskMetadata::generationId's
        // comments). Two store() calls for byte-identical content
        // therefore always publish under two DIFFERENT generation
        // filenames: the second call can never rewrite the first's
        // already-live files in place, so a failure partway through the
        // second call's own publication can never corrupt or delete
        // bytes the first call already made durably live. An existing,
        // currently-live generation is therefore fully intact at every
        // point up until the manifest swap below actually commits. See
        // the class comment for the full crash-consistency argument.
        const std::optional<QString> previousGeneration =
            readManifestGeneration(key);
        // Review item 11: this access sequence is minted once and reused
        // both as the metadata's own recency witness AND as part of the
        // generation identifier's entropy material -- a single genuine
        // access/store deserves exactly one sequence value.
        const quint64 accessSequence = nextAccessSequenceLocked();
        QString generation = mintGenerationIdLocked(accessSequence);
        // Vanishingly unlikely (a 256-bit hash collision), but never
        // silently reuse an already-live generation's filename: remint
        // until it's confirmed distinct from whatever is currently
        // published for this key.
        while (previousGeneration && *previousGeneration == generation) {
          generation = mintGenerationIdLocked(nextAccessSequenceLocked());
        }
        const QString genPayloadPath = generationPayloadPath(key, generation);
        const QString genMetadataPath = generationMetadataPath(key, generation);

        // Phase 1: write this generation's own two files (a temp name +
        // fsync + renameat each, via writeFileAtomicRelative()) -- neither
        // is visible under its FINAL name until its own rename commits,
        // and neither one touches any OTHER generation's files at all.
        bool phase1Ok = writeFileAtomicRelative(
            m_rootFd, genPayloadPath, entry.encodedBytes, /*durable=*/true);
        if (phase1Ok) {
          DiskMetadata metadata;
          metadata.key = key;
          metadata.contentType = entry.contentType;
          metadata.encodedSize = entry.encodedBytes.size();
          metadata.width = entry.dimensions.width();
          metadata.height = entry.dimensions.height();
          metadata.sha256Hex = entry.sha256Hex;
          metadata.generationId = generation;
          metadata.etag = entry.etag;
          metadata.lastModified = entry.lastModified;
          metadata.insertedAtMsecsSinceEpoch = entry.insertedAtMsecsSinceEpoch;
          metadata.lastAccessMsecsSinceEpoch = entry.lastAccessMsecsSinceEpoch;
          metadata.accessSequence = accessSequence;
          phase1Ok = writeMetadata(genMetadataPath, metadata);
          if (!phase1Ok) {
            // Metadata is the sole validity witness for a payload (see
            // lookupDisk()): without it, the payload just committed can
            // never be read back as valid. Delete it immediately rather
            // than leaving that window open; the OLD generation (if any)
            // is completely untouched by any of this.
            removeFileRelative(m_rootFd, genPayloadPath);
          }
        }

        if (phase1Ok) {
          // Round-4/5 review item 4/10: REQUIRED barrier #1 -- fsync the
          // root directory NOW, capturing BOTH of this generation's own
          // renames, BEFORE the manifest (the pointer that actually makes
          // this generation "live") is even written. Without this barrier,
          // a real power-loss between the manifest's rename and the
          // filesystem journal actually committing the payload/metadata
          // renames could leave the manifest pointing at a generation
          // whose own files were never made durable. If this fsync
          // fails, this generation's files are not yet the one anything
          // points to -- discard them as orphans and leave whatever was
          // previously live completely untouched.
          if (!fsyncRootLocked()) {
            qWarning() << "AssetCache: directory fsync failed after writing"
                       << "new generation for" << key
                       << "-- discarding it as an orphan";
            removeFileRelative(m_rootFd, genPayloadPath);
            removeFileRelative(m_rootFd, genMetadataPath);
            phase1Ok = false;
          }
        }

        if (phase1Ok) {
          // Phase 2: the single atomic pointer swap that actually
          // publishes this generation as the live one for `key`.
          if (writeManifest(key, generation)) {
            // Round-4/5 review item 4/10: REQUIRED barrier #2 -- fsync the
            // root directory again, capturing the manifest's own rename.
            // The manifest rename ITSELF is already committed at the
            // filesystem level at this point -- there is no reliable way
            // to "roll it back" if this fsync fails. What CAN still be
            // controlled is whether the OLD generation's files are
            // reclaimed: if this fsync fails, this crash-durability
            // barrier has NOT actually been crossed, so the old
            // generation is deliberately kept around as a durability
            // hedge (a crash right after this point could otherwise leave
            // NEITHER generation recoverable if the rename itself hadn't
            // actually reached stable storage) rather than reclaimed as
            // if publication were guaranteed durable.
            if (fsyncRootLocked()) {
              // Only now -- strictly after the new generation is
              // confirmed durably live via TWO separate directory
              // fsyncs -- reclaim the OLD generation's files, if there
              // was a different one. A crash at any point before this
              // line leaves the OLD generation still fully intact and
              // still the one the manifest names (recovery -- see
              // reapAndEnforceQuota() -- always resolves to whichever
              // generation the manifest currently names, never a
              // half-valid mix of the two).
              if (previousGeneration && *previousGeneration != generation) {
                removeFileRelative(
                    m_rootFd, generationPayloadPath(key, *previousGeneration));
                removeFileRelative(
                    m_rootFd, generationMetadataPath(key, *previousGeneration));
                // Review round-4/5 item 10: fsync once more after
                // reclaiming the old generation, so that reclamation
                // itself is durable and a crash immediately after it
                // cannot resurrect the old generation's now-unlinked
                // directory entries via a stale journal replay.
                (void)fsyncRootLocked();
              }
            } else {
              qWarning()
                  << "AssetCache: directory fsync failed after publishing"
                  << key
                  << "-- retaining previous generation as a durability hedge";
            }
          } else {
            // Manifest publish failed: this generation's files become
            // orphans, safely reclaimed by the next reap sweep (or
            // immediately here, defensively) -- the OLD generation (if
            // any) remains untouched and still live.
            removeFileRelative(m_rootFd, genPayloadPath);
            removeFileRelative(m_rootFd, genMetadataPath);
          }
        }
      }
#endif
    }
  }

  // A cheap, stat-only usage check (diskUsageBytes(): sums fstatat-based
  // sizes across the directory via sumUsageRelative(), no payload reads
  // or re-hashing) gates the much more expensive full reap/validate
  // sweep below: reapAndEnforceQuota()
  // re-reads and re-hashes EVERY cached payload to validate its
  // payload/metadata pair, which makes it O(N * entry_size) over the whole
  // cache -- unconditionally running that on every single store() call
  // would make each individual asset store's cost scale with total cache
  // size, not just that one asset's size. The constructor already runs one
  // unconditional sweep at startup (to repair anything a prior crash left
  // as an orphan/half-written pair), so skipping the sweep here whenever
  // usage is still comfortably under quota never leaves genuine corruption
  // undetected forever -- only until either this check trips or the next
  // process start.
  const qint64 highWaterMark =
      static_cast<qint64>(m_config.diskMaxBytes * kHighWaterMarkFraction);
  if (!m_diskCacheDisabled && diskUsageBytes() > highWaterMark) {
    reapAndEnforceQuota();
  }
}

void AssetCache::touchAfterNotModified(const QString &key,
                                       const QString &newEtag,
                                       const QString &newLastModified) {
  if (m_diskCacheDisabled) {
    return;
  }
  if (!isValidKey(key)) {
    // Never let a malformed key reach manifestPath()/
    // generationMetadataPath() below -- see isValidKey()'s comment.
    return;
  }
  QMutexLocker locker(&m_mutex);
  if (!verifyRootAnchorLocked()) {
    return;
  }
  const std::optional<QString> generation = readManifestGeneration(key);
  const std::optional<DiskMetadata> metadata =
      generation ? readMetadata(generationMetadataPath(key, *generation), key)
                 : std::nullopt;
  if (!generation || !metadata || metadata->generationId != *generation) {
    // Manifest/metadata missing, corrupt, or self-inconsistent, exactly
    // as lookupDisk() and reapAndEnforceQuota() both handle: a caller
    // only reaches touchAfterNotModified() after a 304 response to a
    // conditional request it issued for what it believed was a valid
    // disk entry, so this is itself a repair signal, not merely
    // "already evicted." Clean up defensively for the same reason
    // lookupDisk() does; a subsequent lookup will simply miss and
    // refetch from scratch.
    (void)deleteEntry(key);
    return;
  }
  DiskMetadata refreshed = *metadata;
  refreshed.lastAccessMsecsSinceEpoch = QDateTime::currentMSecsSinceEpoch();
  // Review item 11: a successful conditional revalidation is a genuine
  // access -- mint a fresh monotonic sequence exactly like a full
  // disk/memory hit does.
  refreshed.accessSequence = nextAccessSequenceLocked();
  if (!newEtag.isEmpty()) {
    refreshed.etag = newEtag;
  }
  if (!newLastModified.isEmpty()) {
    refreshed.lastModified = newLastModified;
  }
  // The generation itself (content hash) never changes on a 304 --
  // only its metadata (etag/lastModified/lastAccess) is rewritten in
  // place, atomically, via the same generation-scoped file; the
  // manifest is untouched since the live generation hasn't changed.
  (void)writeMetadata(generationMetadataPath(key, *generation), refreshed);

  if (CachedEntry *entry = m_memory->object(key)) {
    entry->lastAccessMsecsSinceEpoch = refreshed.lastAccessMsecsSinceEpoch;
    if (!newEtag.isEmpty()) {
      entry->etag = newEtag;
    }
    if (!newLastModified.isEmpty()) {
      entry->lastModified = newLastModified;
    }
  }
}

void AssetCache::updateMemoryDecodedImage(const QString &key,
                                          const QImage &image) {
  QMutexLocker locker(&m_mutex);
  CachedEntry *existing = m_memory->object(key);
  if (!existing) {
    return; // evicted since the caller's lookup; a later lookup redecodes
  }
  // A full copy-then-reinsert (rather than mutating *existing in place)
  // is required here, unlike touchAfterNotModified()'s in-place field
  // updates: decodedImage directly affects costBytes(), and QCache's
  // internal cost accounting is only ever updated via insert(), never by
  // mutating an object it already owns.
  auto *updated = new CachedEntry(*existing);
  updated->decodedImage = image;
  m_memory->insert(key, updated, static_cast<qsizetype>(updated->costBytes()));
}

void AssetCache::promoteToMemory(const QString &key, CachedEntry entry) {
  if (!isValidKey(key)) {
    // Unlike lookupDisk()/store()/touchAfterNotModified(), this method
    // never turns `key` into a filesystem path, so a malformed key can't
    // cause a path-traversal here -- but it's a public API, and every
    // other entry point enforces the invariant that entries only ever
    // exist in the cache under a cacheKeyFor()-shaped (64-hex) key.
    // Silently accepting anything else here would let this one method
    // create an inconsistency future callers/entry points can't rely on
    // -- see isValidKey()'s comment.
    return;
  }
  QMutexLocker locker(&m_mutex);
  auto *heapEntry = new CachedEntry(std::move(entry));
  m_memory->insert(key, heapEntry,
                   static_cast<qsizetype>(heapEntry->costBytes()));
}

AssetCache::InvalidateResult AssetCache::invalidate(const QString &key) {
  ++m_invalidateCallCountForTesting;
  if (!isValidKey(key)) {
    return InvalidateResult::DurablyInvalidated;
  }
  QMutexLocker locker(&m_mutex);
  m_memory->remove(key);
  const DeleteEntryOutcome outcome = deleteEntry(key);
  return outcome.manifestDurablyAbsent ? InvalidateResult::DurablyInvalidated
                                       : InvalidateResult::PersistenceFailed;
}

void AssetCache::reapAndEnforceQuota() {
  if (m_diskCacheDisabled) {
    // Review item 7: root was a symlink at construction -- never
    // enumerate, follow, or delete anything through it.
    return;
  }
  QMutexLocker locker(&m_mutex);
  if (!verifyRootAnchorLocked()) {
    // Review round-3 item 9: root replaced since construction -- never
    // enumerate, follow, or delete anything through it.
    return;
  }

#if defined(Q_OS_UNIX)
  if (m_rootFd < 0) {
    return;
  }
  // Round-4/5 review item 3: enumerate relative to `m_rootFd`, never a
  // QDir re-resolving `m_directory` by path. Every entry directly under
  // the root -- files, directories, and symlink nodes (including a
  // DANGLING symlink whose target no longer exists) -- is visible here,
  // since fstatat(..., AT_SYMLINK_NOFOLLOW) reports the entry's own
  // on-disk type without ever resolving through it; a stray directory or
  // hidden dotfile is exactly as much of a leftover as a stray file, and
  // must not be invisible to this repair sweep.
  const std::vector<RelativeDirEntry> allEntries =
      listAllEntriesRelative(m_rootFd);
  QStringList allFiles;
  allFiles.reserve(static_cast<qsizetype>(allEntries.size()));
  for (const RelativeDirEntry &entry : allEntries) {
    // Review item 7: classify via the OWN-type stat above (never a
    // second, path-based, potentially-resolving stat). A symlink node
    // found directly inside the cache root -- to a directory, a file,
    // or a dangling target -- is unlinked itself (an unlinkat(), never a
    // recursive follow); its target is never opened, stat'd through, or
    // touched. Only a genuine (non-symlink) directory is recursed into,
    // and even then via safeRemoveTreeRelative()'s descriptor-relative,
    // device-checked, no-follow walk (review round-4/5 item 9) rather
    // than anything that could itself follow a symlink or cross a mount
    // boundary.
    if (entry.isSymlinkNode) {
      removeFileRelative(m_rootFd, entry.name);
      continue;
    }
    if (entry.isDirectory) {
      safeRemoveTreeRelative(
          m_rootFd, entry.name,
          MountIdentity{m_rootDevice, m_rootMountId, m_rootHasMountId});
      continue;
    }
    allFiles.append(entry.name);
  }
#else
  return;
#endif

  // First pass: discover every manifest (key -> the generation it
  // names) and every generation-scoped payload/metadata file present
  // (key -> the set of generations found for each shape), removing
  // anything that doesn't match one of the three known filename shapes
  // (review item 8) outright.
  QHash<QString, QString> manifestGeneration;
  QHash<QString, QSet<QString>> payloadGenerations;
  QHash<QString, QSet<QString>> metadataGenerations;
  for (const QString &name : allFiles) {
    if (const auto m = manifestNamePattern().match(name); m.hasMatch()) {
      const QString key = m.captured(1);
      if (const std::optional<QString> generation =
              readManifestGeneration(key)) {
        manifestGeneration[key] = *generation;
      } else {
        // Manifest itself unreadable/malformed/self-inconsistent: a
        // stray leftover -- remove outright. Any generation files it
        // might have named are reclaimed below as orphans, since no
        // valid manifest ends up naming this key.
#if defined(Q_OS_UNIX)
        removeFileRelative(m_rootFd, name);
#endif
      }
      continue;
    }
    if (const auto m = generationPayloadNamePattern().match(name);
        m.hasMatch()) {
      payloadGenerations[m.captured(1)].insert(m.captured(2));
      continue;
    }
    if (const auto m = generationMetadataNamePattern().match(name);
        m.hasMatch()) {
      metadataGenerations[m.captured(1)].insert(m.captured(2));
      continue;
    }
    // Anything not matching one of the three known shapes is a stray
    // leftover (a writeFileAtomicRelative() temp artifact from an
    // interrupted write, or debris from an old format) -- this
    // directory is exclusively owned by AssetCache, so removing it
    // unconditionally is safe.
#if defined(Q_OS_UNIX)
    removeFileRelative(m_rootFd, name);
#endif
  }

  QSet<QString> allKeys;
  for (auto it = manifestGeneration.constBegin();
       it != manifestGeneration.constEnd(); ++it) {
    allKeys.insert(it.key());
  }
  for (auto it = payloadGenerations.constBegin();
       it != payloadGenerations.constEnd(); ++it) {
    allKeys.insert(it.key());
  }
  for (auto it = metadataGenerations.constBegin();
       it != metadataGenerations.constEnd(); ++it) {
    allKeys.insert(it.key());
  }

  struct ValidEntry {
    QString key;
    qint64 totalBytes;
    qint64 lastAccessMs;
    // Review item 11: the primary eviction-ordering key (see the class
    // comment) -- monotonic and unique-at-write-time for any entry ever
    // written by this fix, so lastAccessMs only ever matters as a
    // tie-break for legacy (pre-this-fix) entries that still carry the
    // default 0.
    quint64 accessSequence;
  };
  std::vector<ValidEntry> valid;
  // Review item 11: recovers this process's monotonic access-sequence
  // counter from whatever the highest value any entry on disk already
  // carries, so a fresh AssetCache instance (a real process restart)
  // never reissues a sequence number an earlier instance already
  // persisted. Accumulated across every valid entry found in this same
  // sweep, below.
  quint64 maxObservedAccessSequence = 0;

  for (const QString &key : allKeys) {
    const auto manifestIt = manifestGeneration.constFind(key);
    if (manifestIt == manifestGeneration.constEnd()) {
      // No valid manifest names this key at all: nothing for it can be
      // trusted (see the class comment) -- reclaim every generation
      // file left behind for it.
      (void)deleteEntry(key);
      continue;
    }
    const QString &generation = manifestIt.value();
    const bool payloadPresent =
        payloadGenerations.value(key).contains(generation);
    const bool metadataPresent =
        metadataGenerations.value(key).contains(generation);
    if (!payloadPresent || !metadataPresent) {
      // The manifest names a generation whose files are (partially or
      // wholly) missing -- corrupt/incomplete, never a half-valid hit.
      (void)deleteEntry(key);
      continue;
    }
    const QString genPayloadPath = generationPayloadPath(key, generation);
    const QString genMetadataPath = generationMetadataPath(key, generation);
    const std::optional<DiskMetadata> metadata =
        readMetadata(genMetadataPath, key);
#if defined(Q_OS_UNIX)
    const std::optional<QByteArray> verifiedBytes =
        metadata
            ? readExactSizeVerifiedRelative(
                  m_rootFd,
                  MountIdentity{m_rootDevice, m_rootMountId, m_rootHasMountId},
                  genPayloadPath, metadata->encodedSize,
                  kMaxSinglePayloadBytesOnDisk)
            : std::nullopt;
#else
    const std::optional<QByteArray> verifiedBytes = std::nullopt;
#endif
    if (!metadata || metadata->generationId != generation || !verifiedBytes) {
      (void)deleteEntry(key); // corrupt pair, or rejected on size alone
      continue;
    }
    const QByteArray &bytes = *verifiedBytes;
    const QString actualSha256 = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    if (actualSha256 != metadata->sha256Hex ||
        bytes.size() != metadata->encodedSize) {
      (void)deleteEntry(key); // corrupt pair
      continue;
    }

    // The live generation validated cleanly: reclaim any OTHER
    // generation's files still sitting around for this key -- orphans
    // left by a crash between writing a new generation and cleaning up
    // its predecessor, or between writing a new generation's own files
    // and ever reaching the manifest swap (see store()).
#if defined(Q_OS_UNIX)
    for (const QString &other : payloadGenerations.value(key)) {
      if (other != generation) {
        removeFileRelative(m_rootFd, generationPayloadPath(key, other));
      }
    }
    for (const QString &other : metadataGenerations.value(key)) {
      if (other != generation) {
        removeFileRelative(m_rootFd, generationMetadataPath(key, other));
      }
    }
#endif

#if defined(Q_OS_UNIX)
    const auto sizeOfRelative = [this](const QString &name) -> qint64 {
      struct stat st {};
      const QByteArray nameUtf8 = name.toUtf8();
      if (fstatat(m_rootFd, nameUtf8.constData(), &st, AT_SYMLINK_NOFOLLOW) !=
          0) {
        return 0;
      }
      return static_cast<qint64>(st.st_size);
    };
    const qint64 manifestBytes = sizeOfRelative(manifestPath(key));
    const qint64 payloadBytes = sizeOfRelative(genPayloadPath);
    const qint64 metadataBytes = sizeOfRelative(genMetadataPath);
#else
    const qint64 manifestBytes = 0;
    const qint64 payloadBytes = 0;
    const qint64 metadataBytes = 0;
#endif
    valid.push_back(ValidEntry{
        key, manifestBytes + payloadBytes + metadataBytes,
        metadata->lastAccessMsecsSinceEpoch, metadata->accessSequence});
    maxObservedAccessSequence =
        qMax(maxObservedAccessSequence, metadata->accessSequence);
  }

  // Review item 11: never DECREASE the counter -- only ever advance it
  // to strictly past the highest value observed on disk. If this
  // process has already minted sequence values more recent than
  // anything currently on disk (e.g. this sweep ran mid-process after
  // recent memory-only touches whose disk bump is itself in this same
  // batch and therefore already reflected above), m_nextAccessSequence
  // must not regress.
  m_nextAccessSequence =
      qMax(m_nextAccessSequence, maxObservedAccessSequence + 1);

  qint64 totalBytes = diskUsageBytesLocked();
  // Review round-3 item 11: the eviction-target math below is driven by
  // a TRUE, unconditional inventory of every byte still actually present
  // under the root (diskUsageBytesLocked()), not merely the sum of
  // entries that happened to validate cleanly above. A stray/orphan/
  // corrupt entry the cleanup pass above FAILED to remove (a permission
  // error, an undeletable node, or any other reclaim failure) still
  // occupies real, countable disk space; summing only `valid` would
  // silently ignore it and could let quota enforcement report "already
  // under budget" while genuinely undeletable bytes remain resident.

  const qint64 highWaterMark =
      static_cast<qint64>(m_config.diskMaxBytes * kHighWaterMarkFraction);
  const qint64 lowWaterMark =
      static_cast<qint64>(m_config.diskMaxBytes * kLowWaterMarkFraction);

  if (totalBytes <= highWaterMark) {
    return;
  }

  // Oldest access first: evict until at or below the low-water mark.
  // Review item 11: primary key is the monotonic access sequence, NOT
  // raw wall-clock time -- two entries stored/touched within the same
  // millisecond (routine on a fast machine, or in a fast test) are still
  // deterministically ordered by which one was actually accessed first,
  // never by whatever order QHash/QSet happened to iterate `allKeys` in.
  // lastAccessMs and then the cache key itself are tie-breaks, reached
  // only for legacy entries that share the default accessSequence of 0.
  std::sort(valid.begin(), valid.end(),
            [](const ValidEntry &a, const ValidEntry &b) {
              if (a.accessSequence != b.accessSequence) {
                return a.accessSequence < b.accessSequence;
              }
              if (a.lastAccessMs != b.lastAccessMs) {
                return a.lastAccessMs < b.lastAccessMs;
              }
              return a.key < b.key;
            });

  for (const ValidEntry &e : valid) {
    if (totalBytes <= lowWaterMark) {
      break;
    }
    // Review item 11: only credit quota accounting with this entry's
    // bytes as freed if its files were actually confirmed removed. A
    // failed deletion (e.g. a permission error, or a hostile/corrupt
    // entry replaced by an undeletable node) leaves the entry counted as
    // still occupying its space and still resident in memory (it is, in
    // fact, still fully valid on disk) -- moving on to the next
    // candidate rather than looping forever on the one that can't be
    // reclaimed.
    if (deleteEntry(e.key).allFilesReclaimed) {
      m_memory->remove(e.key);
      totalBytes -= e.totalBytes;
    }
  }
}

qint64 AssetCache::memoryCostBytes() const {
  QMutexLocker locker(&m_mutex);
  return m_memory->totalCost();
}

qint64 AssetCache::diskUsageBytes() const {
  QMutexLocker locker(&m_mutex);
  // Recurses into subdirectories (see sumUsageRelative()), not a flat
  // listing -- this directory is exclusively owned by AssetCache, so a
  // stray directory (e.g. planted, or otherwise left behind) should
  // never be invisible to this usage total. This cheap, stat-only check
  // gates whether the much more expensive reapAndEnforceQuota() sweep
  // (which does recursively remove stray directories) runs at all in
  // store()'s hot path -- if a stray directory's contents weren't
  // counted here, that gate could never trip, and the sweep that would
  // actually clean it up might never run. Hidden entries are included
  // too, so a stray hidden file would never undercount usage or prevent
  // the high-water-mark gate from ever tripping. Review round-3 item 9:
  // routed through diskUsageBytesLocked(), which itself re-verifies the
  // root anchor (device+inode) before enumerating anything -- see
  // verifyRootAnchorLocked()'s comment.
  return diskUsageBytesLocked();
}

int AssetCache::diskEntryCount() const {
  QMutexLocker locker(&m_mutex);
  if (!verifyRootAnchorLocked()) {
    return 0;
  }
#if defined(Q_OS_UNIX)
  if (m_rootFd < 0) {
    return 0;
  }
  int count = 0;
  // Round-4/5 review item 3: enumerated relative to `m_rootFd`, never a
  // QDir re-resolving `m_directory` by path.
  for (const RelativeDirEntry &entry : listAllEntriesRelative(m_rootFd)) {
    // One manifest file exists per live entry (review item 8) -- this is
    // a more accurate "how many entries does this cache have" count than
    // counting generation-scoped metadata files, which can transiently
    // include an orphan left by an interrupted replacement until the
    // next reap sweep reclaims it.
    if (!entry.isDirectory && !entry.isSymlinkNode &&
        manifestNamePattern().match(entry.name).hasMatch()) {
      ++count;
    }
  }
  return count;
#else
  return 0;
#endif
}

std::optional<quint64>
AssetCache::accessSequenceForTesting(const QString &key) const {
  QMutexLocker locker(&m_mutex);
  if (m_diskCacheDisabled || !isValidKey(key)) {
    return std::nullopt;
  }
  const std::optional<QString> generation = readManifestGeneration(key);
  if (!generation) {
    return std::nullopt;
  }
  const std::optional<DiskMetadata> metadata =
      readMetadata(generationMetadataPath(key, *generation), key);
  if (!metadata || metadata->generationId != *generation) {
    return std::nullopt;
  }
  return metadata->accessSequence;
}

std::optional<QString>
AssetCache::currentGenerationForTesting(const QString &key) const {
  QMutexLocker locker(&m_mutex);
  if (m_diskCacheDisabled || !isValidKey(key)) {
    return std::nullopt;
  }
  return readManifestGeneration(key);
}

bool AssetCache::directoryChainResolvesNoFollowForTesting(
    const QString &trustedAnchorPath,
    const QStringList &ownedSuffixComponents) {
#if defined(Q_OS_UNIX)
  const std::optional<int> fd =
      openDirectoryChainNoFollow(trustedAnchorPath, ownedSuffixComponents);
  if (fd) {
    ::close(*fd);
  }
  return fd.has_value();
#else
  Q_UNUSED(trustedAnchorPath);
  Q_UNUSED(ownedSuffixComponents);
  return false;
#endif
}

bool AssetCache::resolveTrustedDirectoryNoFollowForTesting(
    const QString &absoluteTargetPath, bool allowCreateMissingComponents) {
#if defined(Q_OS_UNIX)
  const std::optional<int> fd = resolveTrustedDirectoryNoFollow(
      absoluteTargetPath, allowCreateMissingComponents);
  if (fd) {
    ::close(*fd);
  }
  return fd.has_value();
#else
  Q_UNUSED(absoluteTargetPath);
  Q_UNUSED(allowCreateMissingComponents);
  return false;
#endif
}

bool AssetCache::mountIdentificationSupportedForTesting(const QString &path) {
#if defined(__linux__) && defined(STATX_MNT_ID)
  const QByteArray pathUtf8 = QFile::encodeName(path);
  const int fd =
      ::open(pathUtf8.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  const MountIdentity identity = mountIdentityForFd(fd);
  ::close(fd);
  return identity.hasMountId;
#else
  Q_UNUSED(path);
  return false;
#endif
}

} // namespace Arkham
