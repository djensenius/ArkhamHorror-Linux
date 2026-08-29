#include "TokenEnvelope.h"

#include "TokenValidation.h"

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {
constexpr auto kMagicPrefix = "AHKV"_L1;
constexpr int kSupportedVersion = 1;

// Returns the malformed sentinel result. A small helper purely to keep
// every early-return site below identically shaped and obviously correct
// (never accidentally populating endpointIdentity/token on a failure
// path).
TokenEnvelopeParseResult malformed() {
  TokenEnvelopeParseResult result;
  result.outcome = TokenEnvelopeParseOutcome::Malformed;
  return result;
}
} // namespace

QString serializeTokenEnvelope(const QString &endpointIdentity,
                               const QString &token) {
  return kMagicPrefix + QString::number(kSupportedVersion) + u':' +
         QString::number(endpointIdentity.size()) + u':' + endpointIdentity +
         token;
}

TokenEnvelopeParseResult parseTokenEnvelope(const QString &raw) {
  if (!raw.startsWith(kMagicPrefix)) {
    TokenEnvelopeParseResult result;
    result.outcome = TokenEnvelopeParseOutcome::LegacyUnbound;
    return result;
  }

  // Parse the decimal version digits immediately after the magic prefix,
  // up to (but not including) the first ':'.
  qsizetype pos = kMagicPrefix.size();
  qsizetype versionEnd = pos;
  while (versionEnd < raw.size() && raw.at(versionEnd).isDigit()) {
    ++versionEnd;
  }
  if (versionEnd == pos || versionEnd >= raw.size() ||
      raw.at(versionEnd) != u':') {
    // Either no version digits at all, or no ':' terminator was found --
    // this shares the magic prefix with a real envelope but is not
    // actually one. Treated conservatively as Malformed (not
    // LegacyUnbound) since a genuine pre-envelope legacy token
    // coincidentally starting with "AHKV" is vanishingly unlikely, and
    // corrupt envelope-shaped data must never be silently reinterpreted
    // as a trustworthy legacy token either way -- both outcomes discard
    // the token, so the distinction is purely diagnostic.
    return malformed();
  }
  bool versionOk = false;
  const int version =
      QStringView(raw).sliced(pos, versionEnd - pos).toInt(&versionOk);
  if (!versionOk || version != kSupportedVersion) {
    return malformed();
  }
  pos = versionEnd + 1; // skip ':'

  // Parse the decimal endpointIdentity length prefix the same way.
  qsizetype lenEnd = pos;
  while (lenEnd < raw.size() && raw.at(lenEnd).isDigit()) {
    ++lenEnd;
  }
  if (lenEnd == pos || lenEnd >= raw.size() || raw.at(lenEnd) != u':') {
    return malformed();
  }
  bool lengthOk = false;
  const qint64 identityLength =
      QStringView(raw).sliced(pos, lenEnd - pos).toLongLong(&lengthOk);
  if (!lengthOk || identityLength <= 0) {
    // A genuine save always writes a non-empty endpointIdentity (see
    // QtKeychainTokenStore::saveToken()'s own validation), so a declared
    // length of zero (or a negative/unparsable value) can never be a real
    // envelope.
    return malformed();
  }

  const qsizetype identityStart = lenEnd + 1;
  const qsizetype remaining = raw.size() - identityStart;
  if (identityLength > remaining) {
    // Declared identity length runs past the end of the payload --
    // truncated/corrupt data.
    return malformed();
  }

  const QString endpointIdentity =
      raw.mid(identityStart, static_cast<qsizetype>(identityLength));
  const QString token =
      raw.mid(identityStart + static_cast<qsizetype>(identityLength));
  if (!isValidTokenContent(token)) {
    // QtKeychainTokenStore::saveToken() rejects any token failing this
    // exact shared check (see TokenValidation.h) before ever calling
    // serializeTokenEnvelope(), so a genuine save always writes a token
    // that would still pass here. A remainder failing it now -- whether
    // empty (the payload was truncated right after the identity),
    // whitespace-only, leading/trailing-whitespace, containing an
    // embedded control character, or containing any other character this
    // shared validator rejects -- means the entry is corrupt/tampered.
    // None of these are ever a real entry.
    return malformed();
  }

  TokenEnvelopeParseResult result;
  result.outcome = TokenEnvelopeParseOutcome::Parsed;
  result.endpointIdentity = endpointIdentity;
  result.token = token;
  return result;
}

} // namespace Arkham
