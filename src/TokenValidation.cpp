#include "TokenValidation.h"

namespace Arkham {

bool isValidTokenContent(const QString &token) {
  if (token.isEmpty()) {
    return false;
  }
  for (const QChar &ch : token) {
    const ushort unit = ch.unicode();
    // See TokenValidation.h's own comment for exactly why this single
    // range check (U+0021-U+007E inclusive) is sufficient to reject every
    // disallowed category (ASCII space, C0 controls, DEL, C1 controls,
    // Unicode whitespace, zero-width/format characters, and any other
    // non-ASCII code point) without an explicit per-category allow-list.
    if (unit < 0x21 || unit > 0x7E) {
      return false;
    }
  }
  return true;
}

} // namespace Arkham
