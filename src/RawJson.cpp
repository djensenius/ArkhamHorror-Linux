#include "RawJson.h"

#include <QAnyStringView>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

namespace Arkham::Json {

namespace {

[[nodiscard]] bool isAsciiDigit(char c) noexcept {
  return c >= '0' && c <= '9';
}

[[nodiscard]] bool isHexDigit(char c) noexcept {
  return isAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] int hexValue(char c) noexcept {
  if (isAsciiDigit(c))
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + (c - 'a');
  return 10 + (c - 'A');
}

} // namespace

// Recursive-descent RFC 8259 parser. One Parser is constructed per
// Value::parse() call and owns all of that call's mutable scan state; every
// method advances `m_pos` past what it consumed and returns a Failure on
// the first byte that violates the grammar. `m_limits` bounds every
// resource this parser allocates (see ParseLimits); `m_totalNodes` counts
// every value produced so far against `m_limits.maxTotalNodes`.
class Parser {
public:
  Parser(QByteArrayView bytes, QStringView path, const ParseLimits &limits)
      : m_bytes(bytes), m_path(path), m_limits(limits) {}

  [[nodiscard]] ValueOrError<Value> parseDocument() {
    if (m_bytes.size() > m_limits.maxInputBytes)
      return Failure{QStringLiteral("%1: input exceeds the maximum allowed "
                                    "size of %2 bytes")
                         .arg(m_path.toString())
                         .arg(m_limits.maxInputBytes)};
    skipWhitespace();
    auto value = parseValue(0);
    if (!value)
      return Failure{value.error()};
    skipWhitespace();
    if (m_pos != m_bytes.size())
      return failAt("trailing content after JSON value");
    return *value;
  }

private:
  QByteArrayView m_bytes;
  QStringView m_path;
  ParseLimits m_limits;
  qsizetype m_pos = 0;
  qsizetype m_totalNodes = 0;

  [[nodiscard]] Failure failAt(QAnyStringView what) const {
    return Failure{QStringLiteral("%1: byte offset %2: %3")
                       .arg(m_path.toString())
                       .arg(m_pos)
                       .arg(what.toString())};
  }

  [[nodiscard]] bool atEnd() const noexcept { return m_pos >= m_bytes.size(); }
  [[nodiscard]] char peek() const noexcept {
    return atEnd() ? '\0' : m_bytes[m_pos];
  }

  void skipWhitespace() noexcept {
    while (!atEnd()) {
      char c = m_bytes[m_pos];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        ++m_pos;
      else
        break;
    }
  }

  [[nodiscard]] ValueOrError<Value> parseValue(int depth) {
    if (depth > m_limits.maxDepth)
      return failAt("nesting depth exceeds the maximum allowed");
    if (++m_totalNodes > m_limits.maxTotalNodes)
      return failAt("document exceeds the maximum allowed total node count");
    if (atEnd())
      return failAt("unexpected end of input, expected a JSON value");
    char c = peek();
    if (c == '"')
      return parseString();
    if (c == '{')
      return parseObject(depth + 1);
    if (c == '[')
      return parseArray(depth + 1);
    if (c == 't')
      return parseLiteral("true"_L1, [] {
        Value v;
        v.m_kind = Value::Kind::Bool;
        v.m_bool = true;
        return v;
      });
    if (c == 'f')
      return parseLiteral("false"_L1, [] {
        Value v;
        v.m_kind = Value::Kind::Bool;
        v.m_bool = false;
        return v;
      });
    if (c == 'n')
      return parseLiteral("null"_L1, [] {
        Value v;
        v.m_kind = Value::Kind::Null;
        return v;
      });
    if (c == '-' || isAsciiDigit(c))
      return parseNumber();
    return failAt(
        QStringLiteral("unexpected character '%1'").arg(QChar::fromLatin1(c)));
  }

  template <typename MakeValue>
  [[nodiscard]] ValueOrError<Value> parseLiteral(QLatin1StringView text,
                                                 MakeValue makeValue) {
    if (m_pos + text.size() > m_bytes.size() ||
        m_bytes.sliced(m_pos, text.size()) !=
            QByteArrayView(text.data(), text.size()))
      return failAt(QStringLiteral("invalid literal, expected '%1'")
                        .arg(QString::fromLatin1(text)));
    m_pos += text.size();
    return makeValue();
  }

  // RFC 8259 number = -? int frac? exp?
  [[nodiscard]] ValueOrError<Value> parseNumber() {
    RawNumber number;
    if (peek() == '-') {
      number.m_negative = true;
      ++m_pos;
    }
    if (atEnd() || !isAsciiDigit(peek()))
      return failAt("invalid number: expected a digit");
    qsizetype intStart = m_pos;
    if (peek() == '0') {
      ++m_pos;
    } else {
      while (!atEnd() && isAsciiDigit(peek()))
        ++m_pos;
    }
    if (m_pos - intStart > m_limits.maxNumberDigits)
      return failAt("number literal's integer part exceeds the maximum "
                    "allowed digit count");
    number.m_intDigits =
        QString::fromLatin1(m_bytes.sliced(intStart, m_pos - intStart));
    if (!atEnd() && peek() == '.') {
      ++m_pos;
      qsizetype fracStart = m_pos;
      if (atEnd() || !isAsciiDigit(peek()))
        return failAt("invalid number: expected a digit after '.'");
      while (!atEnd() && isAsciiDigit(peek()))
        ++m_pos;
      if (m_pos - fracStart > m_limits.maxNumberDigits)
        return failAt("number literal's fraction part exceeds the maximum "
                      "allowed digit count");
      number.m_fracDigits =
          QString::fromLatin1(m_bytes.sliced(fracStart, m_pos - fracStart));
    }
    if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
      ++m_pos;
      number.m_hasExponent = true;
      if (!atEnd() && (peek() == '+' || peek() == '-')) {
        number.m_exponentNegative = peek() == '-';
        ++m_pos;
      }
      qsizetype expStart = m_pos;
      if (atEnd() || !isAsciiDigit(peek()))
        return failAt("invalid number: expected a digit in the exponent");
      while (!atEnd() && isAsciiDigit(peek()))
        ++m_pos;
      if (m_pos - expStart > m_limits.maxNumberDigits)
        return failAt("number literal's exponent part exceeds the maximum "
                      "allowed digit count");
      number.m_expDigits =
          QString::fromLatin1(m_bytes.sliced(expStart, m_pos - expStart));
    }
    Value v;
    v.m_kind = Value::Kind::Number;
    v.m_number = std::move(number);
    return v;
  }

  [[nodiscard]] ValueOrError<Value> parseString() {
    auto text = parseStringText();
    if (!text)
      return Failure{text.error()};
    Value v;
    v.m_kind = Value::Kind::String;
    v.m_string = std::move(*text);
    return v;
  }

  // Consumes a JSON string literal (opening through closing quote already
  // positioned at the opening quote) and returns its decoded text,
  // validating escapes, surrogate pairs, and raw UTF-8 byte sequences.
  [[nodiscard]] ValueOrError<QString> parseStringText() {
    if (peek() != '"')
      return failAt("expected '\"'");
    ++m_pos;
    QString out;
    for (;;) {
      if (atEnd())
        return failAt("unterminated string literal");
      unsigned char c = static_cast<unsigned char>(m_bytes[m_pos]);
      // The closing quote is checked before the length bound below so an
      // already-at-the-limit string that is about to terminate is never
      // wrongly rejected.
      if (c == '"') {
        ++m_pos;
        return out;
      }
      // A hard bound checked before consuming/appending anything else
      // this iteration, guaranteeing `out` never grows past
      // maxStringLength via a single-code-unit append. Branches that
      // append two UTF-16 code units at once (a \u surrogate pair, or a
      // decoded UTF-8 codepoint above the BMP) additionally re-check
      // below, since this alone only guarantees one code unit of
      // headroom.
      if (out.size() >= m_limits.maxStringLength)
        return failAt("string literal exceeds the maximum allowed length");
      if (c == '\\') {
        ++m_pos;
        if (atEnd())
          return failAt("unterminated escape sequence");
        char esc = m_bytes[m_pos];
        switch (esc) {
        case '"':
          out += QChar::fromLatin1('"');
          ++m_pos;
          break;
        case '\\':
          out += QChar::fromLatin1('\\');
          ++m_pos;
          break;
        case '/':
          out += QChar::fromLatin1('/');
          ++m_pos;
          break;
        case 'b':
          out += QChar::fromLatin1('\b');
          ++m_pos;
          break;
        case 'f':
          out += QChar::fromLatin1('\f');
          ++m_pos;
          break;
        case 'n':
          out += QChar::fromLatin1('\n');
          ++m_pos;
          break;
        case 'r':
          out += QChar::fromLatin1('\r');
          ++m_pos;
          break;
        case 't':
          out += QChar::fromLatin1('\t');
          ++m_pos;
          break;
        case 'u': {
          ++m_pos;
          auto unit = parseHex4();
          if (!unit)
            return Failure{unit.error()};
          char16_t code = *unit;
          if (code >= 0xD800 && code <= 0xDBFF) {
            // High surrogate: must be immediately followed by a low
            // surrogate escape, or the pair is malformed.
            if (m_pos + 1 >= m_bytes.size() || m_bytes[m_pos] != '\\' ||
                m_bytes[m_pos + 1] != 'u')
              return failAt("lone high surrogate in \\u escape (missing low "
                            "surrogate)");
            m_pos += 2;
            auto low = parseHex4();
            if (!low)
              return Failure{low.error()};
            if (*low < 0xDC00 || *low > 0xDFFF)
              return failAt("high surrogate not followed by a valid low "
                            "surrogate");
            // This appends two code units at once, so the one-unit
            // headroom already checked above is not enough on its own.
            if (out.size() + 2 > m_limits.maxStringLength)
              return failAt(
                  "string literal exceeds the maximum allowed length");
            out += QChar(code);
            out += QChar(*low);
          } else if (code >= 0xDC00 && code <= 0xDFFF) {
            return failAt("lone low surrogate in \\u escape");
          } else {
            out += QChar(code);
          }
          break;
        }
        default:
          return failAt("invalid escape sequence");
        }
        continue;
      }
      if (c < 0x20)
        return failAt("unescaped control character in string literal");
      if (c < 0x80) {
        out += QChar::fromLatin1(static_cast<char>(c));
        ++m_pos;
        continue;
      }
      auto codepoint = parseUtf8Sequence();
      if (!codepoint)
        return Failure{codepoint.error()};
      char32_t cp = *codepoint;
      if (cp <= 0xFFFF) {
        out += QChar(static_cast<char16_t>(cp));
      } else {
        cp -= 0x10000;
        // A codepoint above the BMP appends two code units at once (a
        // surrogate pair), so re-verify room for both before appending.
        if (out.size() + 2 > m_limits.maxStringLength)
          return failAt("string literal exceeds the maximum allowed length");
        out += QChar(static_cast<char16_t>(0xD800 + (cp >> 10)));
        out += QChar(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
      }
    }
  }

  [[nodiscard]] ValueOrError<char16_t> parseHex4() {
    if (m_pos + 4 > m_bytes.size())
      return failAt("truncated \\u escape, expected 4 hex digits");
    char16_t value = 0;
    for (int i = 0; i < 4; ++i) {
      char c = m_bytes[m_pos + i];
      if (!isHexDigit(c))
        return failAt("invalid hex digit in \\u escape");
      value = static_cast<char16_t>((value << 4) | hexValue(c));
    }
    m_pos += 4;
    return value;
  }

  // Decodes one well-formed UTF-8 sequence starting at m_pos (already known
  // to be a lead byte >= 0x80), rejecting overlong encodings, surrogate
  // code points, out-of-range code points, and truncated/invalid
  // continuation bytes.
  [[nodiscard]] ValueOrError<char32_t> parseUtf8Sequence() {
    unsigned char lead = static_cast<unsigned char>(m_bytes[m_pos]);
    int length;
    char32_t codepoint;
    char32_t minCodepoint;
    if ((lead & 0xE0) == 0xC0) {
      length = 2;
      codepoint = lead & 0x1F;
      minCodepoint = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
      codepoint = lead & 0x0F;
      minCodepoint = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
      codepoint = lead & 0x07;
      minCodepoint = 0x10000;
    } else {
      return failAt("invalid UTF-8 lead byte");
    }
    if (m_pos + length > m_bytes.size())
      return failAt("truncated UTF-8 sequence");
    for (int i = 1; i < length; ++i) {
      unsigned char cont = static_cast<unsigned char>(m_bytes[m_pos + i]);
      if ((cont & 0xC0) != 0x80)
        return failAt("invalid UTF-8 continuation byte");
      codepoint = (codepoint << 6) | (cont & 0x3F);
    }
    m_pos += length;
    if (codepoint < minCodepoint)
      return failAt("overlong UTF-8 encoding");
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
      return failAt("UTF-8 encodes a surrogate code point");
    if (codepoint > 0x10FFFF)
      return failAt("UTF-8 code point out of Unicode range");
    return codepoint;
  }

  [[nodiscard]] ValueOrError<Value> parseArray(int depth) {
    ++m_pos; // '['
    Value v;
    v.m_kind = Value::Kind::Array;
    skipWhitespace();
    if (!atEnd() && peek() == ']') {
      ++m_pos;
      return v;
    }
    for (;;) {
      skipWhitespace();
      auto element = parseValue(depth);
      if (!element)
        return Failure{element.error()};
      if (v.m_array.size() >= m_limits.maxArrayElements)
        return failAt("array exceeds the maximum allowed element count");
      v.m_array.append(std::move(*element));
      skipWhitespace();
      if (atEnd())
        return failAt("unterminated array, expected ',' or ']'");
      if (peek() == ',') {
        ++m_pos;
        continue;
      }
      if (peek() == ']') {
        ++m_pos;
        return v;
      }
      return failAt("expected ',' or ']' in array");
    }
  }

  [[nodiscard]] ValueOrError<Value> parseObject(int depth) {
    ++m_pos; // '{'
    Value v;
    v.m_kind = Value::Kind::Object;
    skipWhitespace();
    if (!atEnd() && peek() == '}') {
      ++m_pos;
      return v;
    }
    for (;;) {
      skipWhitespace();
      if (atEnd() || peek() != '"')
        return failAt("expected a string key in object");
      auto key = parseStringText();
      if (!key)
        return Failure{key.error()};
      if (v.m_objectIndex.contains(*key))
        return failAt(QStringLiteral("duplicate object key '%1'").arg(*key));
      skipWhitespace();
      if (atEnd() || peek() != ':')
        return failAt("expected ':' after object key");
      ++m_pos;
      skipWhitespace();
      auto value = parseValue(depth);
      if (!value)
        return Failure{value.error()};
      if (v.m_object.size() >= m_limits.maxObjectMembers)
        return failAt("object exceeds the maximum allowed member count");
      v.m_objectIndex.insert(*key, v.m_object.size());
      v.m_object.append({std::move(*key), std::move(*value)});
      skipWhitespace();
      if (atEnd())
        return failAt("unterminated object, expected ',' or '}'");
      if (peek() == ',') {
        ++m_pos;
        continue;
      }
      if (peek() == '}') {
        ++m_pos;
        return v;
      }
      return failAt("expected ',' or '}' in object");
    }
  }
};

ValueOrError<Value> Value::parse(QByteArrayView bytes, QStringView path,
                                 const ParseLimits &limits) {
  Parser parser(bytes, path, limits);
  return parser.parseDocument();
}

bool Value::contains(QLatin1StringView key) const {
  if (m_kind != Kind::Object)
    return false;
  return m_objectIndex.contains(QString(key));
}

Value Value::value(QLatin1StringView key) const {
  if (m_kind != Kind::Object)
    return {};
  const auto it = m_objectIndex.constFind(QString(key));
  if (it == m_objectIndex.constEnd())
    return {};
  return m_object[*it].second;
}

QStringList Value::keys() const {
  QStringList result;
  if (m_kind != Kind::Object)
    return result;
  result.reserve(m_object.size());
  for (const auto &[k, v] : m_object)
    result.append(k);
  return result;
}

QJsonValue Value::toQJson() const {
  switch (m_kind) {
  case Kind::Undefined:
    return QJsonValue(QJsonValue::Undefined);
  case Kind::Null:
    return QJsonValue(QJsonValue::Null);
  case Kind::Bool:
    return QJsonValue(m_bool);
  case Kind::Number:
    // Preserve full int64 precision whenever the literal is mathematically
    // integral and in range (see RawNumber::toExactInt64()): Qt's
    // QCborValue-backed QJsonValue(qint64) constructor stores such a value
    // exactly (QJsonValue::toInteger() on the result returns the identical
    // qint64, unlike a value built via the double constructor -- verified
    // against Qt 6.11's QJsonValue/QCborValue implementation). Every other
    // literal (a genuine decimal, or an integral value outside qint64's
    // range) still round-trips only as closely as IEEE-754 double allows.
    if (auto exact = m_number.toExactInt64())
      return QJsonValue(*exact);
    return QJsonValue(m_number.toDouble());
  case Kind::String:
    return QJsonValue(m_string);
  case Kind::Array: {
    QJsonArray array;
    for (const auto &element : m_array)
      array.append(element.toQJson());
    return array;
  }
  case Kind::Object: {
    QJsonObject object;
    for (const auto &[k, v] : m_object)
      object.insert(k, v.toQJson());
    return object;
  }
  }
  return QJsonValue(QJsonValue::Undefined);
}

Value Value::makeNull() {
  Value v;
  v.m_kind = Kind::Null;
  return v;
}

Value Value::makeBool(bool value) {
  Value v;
  v.m_kind = Kind::Bool;
  v.m_bool = value;
  return v;
}

Value Value::makeNumber(RawNumber value) {
  Value v;
  v.m_kind = Kind::Number;
  v.m_number = std::move(value);
  return v;
}

Value Value::makeString(QString value) {
  Value v;
  v.m_kind = Kind::String;
  v.m_string = std::move(value);
  return v;
}

Value Value::makeArray(QList<Value> elements) {
  Value v;
  v.m_kind = Kind::Array;
  v.m_array = std::move(elements);
  return v;
}

Value Value::makeObject(QList<std::pair<QString, Value>> members) {
  Value v;
  v.m_kind = Kind::Object;
  v.m_objectIndex.reserve(members.size());
  for (qsizetype i = 0; i < members.size(); ++i) {
    // First-occurrence wins for a transiently duplicate-keyed input (an
    // invalid state toJsonBytes()'s own duplicate-key check rejects
    // before it could ever be re-serialized), matching contains()/
    // value()'s previous linear-scan behavior of returning the first
    // match exactly.
    if (!v.m_objectIndex.contains(members[i].first))
      v.m_objectIndex.insert(members[i].first, i);
  }
  v.m_object = std::move(members);
  return v;
}

ValueOrError<Value> Value::fromQJson(const QJsonValue &qv) {
  switch (qv.type()) {
  case QJsonValue::Null:
    return makeNull();
  case QJsonValue::Bool:
    return makeBool(qv.toBool());
  case QJsonValue::Double: {
    const double d = qv.toDouble();
    if (!std::isfinite(d))
      return failure(
          QStringLiteral("cannot convert a non-finite number to JSON"));
    if (std::trunc(d) == d) {
      // Always attempt the exact-integer round-trip, regardless of `d`'s
      // magnitude: toInteger() reads the QJsonValue's underlying storage
      // directly rather than approximating through `d`, so it recovers
      // the true stored qint64 even right at the qint64 boundary, where
      // `d` itself is already only an approximation (e.g. qint64::max()
      // rounds UP to 2^63 as a double). A prior version of this function
      // additionally gated this path on `std::abs(d) < 9.2e18`, which
      // excluded exactly that boundary region and silently fell through
      // to the lossy decimal-text fallback below for large-but-valid
      // integers.
      const qint64 asInt = qv.toInteger();
      if (static_cast<double>(asInt) == d)
        return makeNumber(RawNumber::fromInt64(asInt));
    }
    const QString text = QString::number(d, 'g', 17);
    auto parsed = Value::parse(text.toUtf8(), QStringView());
    if (!parsed || !parsed->isNumber())
      return failure(
          QStringLiteral("internal error converting number %1 to JSON")
              .arg(text));
    return *parsed;
  }
  case QJsonValue::String:
    return makeString(qv.toString());
  case QJsonValue::Array: {
    const QJsonArray arr = qv.toArray();
    QList<Value> elements;
    elements.reserve(arr.size());
    for (const auto &e : arr) {
      auto element = fromQJson(e);
      if (!element)
        return failure(element.error());
      elements.append(std::move(*element));
    }
    return makeArray(std::move(elements));
  }
  case QJsonValue::Object: {
    const QJsonObject obj = qv.toObject();
    QList<std::pair<QString, Value>> members;
    members.reserve(obj.size());
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
      auto member = fromQJson(it.value());
      if (!member)
        return failure(member.error());
      members.append({it.key(), std::move(*member)});
    }
    return makeObject(std::move(members));
  }
  case QJsonValue::Undefined:
    return Value{};
  }
  return Value{};
}

namespace {
// UTF-8-encodes and JSON-string-escapes `s`, appending to `out`. Handles
// UTF-16 surrogate pairs (recombining into a single codepoint before
// UTF-8 encoding) and escapes only what RFC 8259 requires (control
// characters, '"', '\\'); every other Unicode character is emitted as
// literal (encoded) UTF-8 bytes, which is valid directly inside a JSON
// string with no escape needed.
void appendJsonEncodedString(QByteArray &out, const QString &s) {
  out += '"';
  for (qsizetype i = 0; i < s.size(); ++i) {
    const QChar ch = s[i];
    char32_t codepoint = ch.unicode();
    if (ch.isHighSurrogate() && i + 1 < s.size() && s[i + 1].isLowSurrogate()) {
      codepoint = QChar::surrogateToUcs4(ch, s[i + 1]);
      ++i;
    }
    switch (codepoint) {
    case '"':
      out += "\\\"";
      continue;
    case '\\':
      out += "\\\\";
      continue;
    case '\b':
      out += "\\b";
      continue;
    case '\f':
      out += "\\f";
      continue;
    case '\n':
      out += "\\n";
      continue;
    case '\r':
      out += "\\r";
      continue;
    case '\t':
      out += "\\t";
      continue;
    default:
      break;
    }
    if (codepoint < 0x20) {
      static constexpr char kHex[] = "0123456789abcdef";
      out += "\\u00";
      out += kHex[(codepoint >> 4) & 0xF];
      out += kHex[codepoint & 0xF];
      continue;
    }
    // Encode as UTF-8 directly; valid inside a JSON string unescaped.
    if (codepoint < 0x80) {
      out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
      out += static_cast<char>(0xC0 | (codepoint >> 6));
      out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
      out += static_cast<char>(0xE0 | (codepoint >> 12));
      out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (codepoint >> 18));
      out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
  }
  out += '"';
}
} // namespace

ValueOrError<QByteArray> Value::toJsonBytes(const ParseLimits &limits) const {
  qsizetype totalNodes = 0;
  return toJsonBytesInner(limits, 0, totalNodes);
}

ValueOrError<QByteArray> Value::toJsonBytesInner(const ParseLimits &limits,
                                                 int depth,
                                                 qsizetype &totalNodes) const {
  if (depth > limits.maxDepth)
    return failure(QStringLiteral(
        "Json::Value::toJsonBytes: nesting depth exceeds the maximum "
        "allowed"));
  if (++totalNodes > limits.maxTotalNodes)
    return failure(QStringLiteral(
        "Json::Value::toJsonBytes: document exceeds the maximum allowed "
        "total node count"));
  switch (m_kind) {
  case Kind::Undefined:
    return failure(QStringLiteral(
        "Json::Value::toJsonBytes: cannot serialize an undefined value"));
  case Kind::Null:
    return QByteArray("null");
  case Kind::Bool:
    return QByteArray(m_bool ? "true" : "false");
  case Kind::Number:
    // A default-constructed RawNumber (m_intDigits empty) is not a value
    // Value::parse() or RawNumber::fromInt64() can ever produce -- it can
    // only arise from a caller directly default-constructing a RawNumber
    // and handing it to Value::makeNumber() -- but literal() would still
    // silently emit an empty, syntactically-invalid token for it with no
    // other check in this function ever catching that. Reject explicitly
    // rather than ever emitting invalid JSON bytes.
    if (m_number.integerDigits().isEmpty())
      return failure(
          QStringLiteral("Json::Value::toJsonBytes: number has no digits"));
    // Mirrors Parser::parseNumber()'s own per-part digit-count check: a
    // programmatically-built RawNumber must not be able to emit an
    // unbounded literal (e.g. via RawNumber::fromInt64() composed with a
    // pathological fraction/exponent no parse() would ever have accepted)
    // any more than a parsed one can.
    if (m_number.integerDigits().size() > limits.maxNumberDigits ||
        m_number.fractionDigits().size() > limits.maxNumberDigits ||
        m_number.exponentDigits().size() > limits.maxNumberDigits)
      return failure(QStringLiteral(
          "Json::Value::toJsonBytes: number literal exceeds the maximum "
          "allowed digit count"));
    return m_number.literal().toUtf8();
  case Kind::String: {
    if (m_string.size() > limits.maxStringLength)
      return failure(QStringLiteral(
          "Json::Value::toJsonBytes: string exceeds the maximum allowed "
          "length"));
    QByteArray out;
    appendJsonEncodedString(out, m_string);
    return out;
  }
  case Kind::Array: {
    if (m_array.size() > limits.maxArrayElements)
      return failure(QStringLiteral(
          "Json::Value::toJsonBytes: array exceeds the maximum allowed "
          "element count"));
    QByteArray out = "[";
    bool first = true;
    for (const auto &element : m_array) {
      if (!first)
        out += ',';
      first = false;
      auto encoded = element.toJsonBytesInner(limits, depth + 1, totalNodes);
      if (!encoded)
        return failure(encoded.error());
      out += *encoded;
    }
    out += ']';
    return out;
  }
  case Kind::Object: {
    if (m_object.size() > limits.maxObjectMembers)
      return failure(QStringLiteral(
          "Json::Value::toJsonBytes: object exceeds the maximum allowed "
          "member count"));
    QByteArray out = "{";
    bool first = true;
    QSet<QString> seenKeys;
    for (const auto &[key, value] : m_object) {
      if (key.size() > limits.maxStringLength)
        return failure(QStringLiteral(
            "Json::Value::toJsonBytes: object key exceeds the maximum "
            "allowed length"));
      // Mirrors Value::parse()'s own duplicate-key rejection: a
      // programmatically-built AST must not be able to emit a duplicate
      // object key any more than a parsed one can (see the class comment
      // on injection safety).
      if (seenKeys.contains(key))
        return failure(
            QStringLiteral(
                "Json::Value::toJsonBytes: duplicate object key '%1'")
                .arg(key));
      seenKeys.insert(key);
      if (!first)
        out += ',';
      first = false;
      appendJsonEncodedString(out, key);
      out += ':';
      auto encoded = value.toJsonBytesInner(limits, depth + 1, totalNodes);
      if (!encoded)
        return failure(encoded.error());
      out += *encoded;
    }
    out += '}';
    return out;
  }
  }
  return failure(QStringLiteral("Json::Value::toJsonBytes: unknown kind"));
}

QString RawNumber::literal() const {
  QString out;
  if (m_negative)
    out += QChar::fromLatin1('-');
  out += m_intDigits;
  if (!m_fracDigits.isEmpty()) {
    out += QChar::fromLatin1('.');
    out += m_fracDigits;
  }
  if (m_hasExponent) {
    out += QChar::fromLatin1('e');
    if (m_exponentNegative)
      out += QChar::fromLatin1('-');
    out += m_expDigits;
  }
  return out;
}

std::optional<qint64> RawNumber::toInt64() const {
  if (hasFraction() || hasExponent())
    return std::nullopt;
  QString text = (m_negative ? QStringLiteral("-") : QString()) + m_intDigits;
  bool ok = false;
  qint64 value = text.toLongLong(&ok);
  if (!ok)
    return std::nullopt;
  return value;
}

namespace {
// Parses an unsigned decimal digit string (no sign) into a qint64
// magnitude, given `negative` to select the valid bound (INT64_MAX for a
// positive value, -INT64_MIN == 9223372036854775808 for a negative one --
// both have exactly 19 digits). Never computes an intermediate value that
// could itself overflow: a digit count above 19 (after stripping leading
// zeros) is rejected outright, and an exact 19-digit string is compared
// digit-by-digit against the bound's text before any arithmetic runs.
std::optional<quint64> parseMagnitudeDigits(QStringView digits, bool negative) {
  qsizetype start = 0;
  while (start < digits.size() - 1 && digits[start] == u'0')
    ++start;
  const QStringView trimmed = digits.mid(start);
  if (trimmed.size() > 19)
    return std::nullopt;
  if (trimmed.size() == 19) {
    const QStringView bound =
        negative ? u"9223372036854775808" : u"9223372036854775807";
    for (qsizetype i = 0; i < 19; ++i) {
      if (trimmed[i] < bound[i])
        break;
      if (trimmed[i] > bound[i])
        return std::nullopt;
    }
  }
  quint64 value = 0;
  for (const QChar ch : trimmed)
    value = value * 10 + static_cast<quint64>(ch.unicode() - u'0');
  return value;
}
} // namespace

std::optional<qint64> RawNumber::toExactInt64() const {
  // Concatenate integer+fraction digits into one unsigned digit string;
  // the decimal point conceptually sits at position m_intDigits.size() in
  // that string, then is shifted further by the exponent (a positive
  // exponent moves it right/toward the end, negative moves it left).
  const QString digits = m_intDigits + m_fracDigits;
  qint64 exponent = 0;
  if (m_hasExponent) {
    bool ok = false;
    // Exponent digit strings are themselves plain unsigned decimal text;
    // one long enough to overflow qint64 unambiguously puts the value's
    // magnitude far outside qint64's range regardless of sign, so treat
    // an unparseable (absurdly long) exponent as simply out of range
    // rather than a distinct error case.
    const qint64 magnitude = m_expDigits.toLongLong(&ok);
    if (!ok)
      return std::nullopt;
    exponent = m_exponentNegative ? -magnitude : magnitude;
  }
  const qint64 decimalPointPos =
      static_cast<qint64>(m_intDigits.size()) + exponent;

  QString magnitudeDigits;
  if (decimalPointPos >= digits.size()) {
    // Every digit lies before the effective point; pad with trailing
    // zeros. Bound the padding itself: anything requiring more than 19
    // digits overall is already out of qint64's range, so there is no
    // need to materialize an arbitrarily long padded string first.
    const qint64 zeroPad = decimalPointPos - digits.size();
    if (zeroPad > 19)
      return std::nullopt;
    magnitudeDigits = digits + QString(static_cast<int>(zeroPad), u'0');
  } else if (decimalPointPos <= 0) {
    // The value's magnitude is < 1 unless every digit is zero.
    for (const QChar c : digits)
      if (c != u'0')
        return std::nullopt;
    magnitudeDigits = u"0"_s;
  } else {
    // Digits at or past the effective point must all be zero for the
    // value to be integral.
    for (qsizetype i = decimalPointPos; i < digits.size(); ++i)
      if (digits[static_cast<qsizetype>(i)] != u'0')
        return std::nullopt;
    magnitudeDigits = digits.left(static_cast<qsizetype>(decimalPointPos));
  }
  if (magnitudeDigits.isEmpty())
    magnitudeDigits = u"0"_s;

  const auto magnitude = parseMagnitudeDigits(magnitudeDigits, m_negative);
  if (!magnitude)
    return std::nullopt;
  if (m_negative) {
    constexpr quint64 kMinMagnitude =
        static_cast<quint64>(std::numeric_limits<qint64>::max()) + 1;
    if (*magnitude == kMinMagnitude)
      return std::numeric_limits<qint64>::min();
    return -static_cast<qint64>(*magnitude);
  }
  if (*magnitude > static_cast<quint64>(std::numeric_limits<qint64>::max()))
    return std::nullopt;
  return static_cast<qint64>(*magnitude);
}

double RawNumber::toDouble() const { return literal().toDouble(); }

RawNumber RawNumber::fromInt64(qint64 value) {
  RawNumber result;
  result.m_negative = value < 0;
  // QString::number(qint64) correctly handles the full range, including
  // std::numeric_limits<qint64>::min() (negating it directly would
  // overflow), so build the signed text once and split off the sign.
  const QString text = QString::number(value);
  result.m_intDigits = result.m_negative ? text.mid(1) : text;
  return result;
}

} // namespace Arkham::Json
