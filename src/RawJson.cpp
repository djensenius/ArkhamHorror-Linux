#include "RawJson.h"

#include <QAnyStringView>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

using namespace Qt::StringLiterals;

namespace Arkham::Json {

namespace {

// Bounds recursion so a pathological (or adversarial) input cannot overflow
// the call stack; see the class comment on Value::parse().
constexpr int kMaxNestingDepth = 200;

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
// the first byte that violates the grammar.
class Parser {
public:
  Parser(QByteArrayView bytes, QStringView path)
      : m_bytes(bytes), m_path(path) {}

  [[nodiscard]] ValueOrError<Value> parseDocument() {
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
  qsizetype m_pos = 0;

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
    if (depth > kMaxNestingDepth)
      return failAt("nesting depth exceeds the maximum allowed");
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
    number.m_intDigits =
        QString::fromLatin1(m_bytes.sliced(intStart, m_pos - intStart));
    if (!atEnd() && peek() == '.') {
      ++m_pos;
      qsizetype fracStart = m_pos;
      if (atEnd() || !isAsciiDigit(peek()))
        return failAt("invalid number: expected a digit after '.'");
      while (!atEnd() && isAsciiDigit(peek()))
        ++m_pos;
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
      if (c == '"') {
        ++m_pos;
        return out;
      }
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
    QSet<QString> seenKeys;
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
      if (seenKeys.contains(*key))
        return failAt(QStringLiteral("duplicate object key '%1'").arg(*key));
      seenKeys.insert(*key);
      skipWhitespace();
      if (atEnd() || peek() != ':')
        return failAt("expected ':' after object key");
      ++m_pos;
      skipWhitespace();
      auto value = parseValue(depth);
      if (!value)
        return Failure{value.error()};
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

ValueOrError<Value> Value::parse(QByteArrayView bytes, QStringView path) {
  Parser parser(bytes, path);
  return parser.parseDocument();
}

bool Value::contains(QLatin1StringView key) const {
  if (m_kind != Kind::Object)
    return false;
  for (const auto &[k, v] : m_object) {
    if (k == key)
      return true;
  }
  return false;
}

Value Value::value(QLatin1StringView key) const {
  if (m_kind != Kind::Object)
    return {};
  for (const auto &[k, v] : m_object) {
    if (k == key)
      return v;
  }
  return {};
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

double RawNumber::toDouble() const { return literal().toDouble(); }

} // namespace Arkham::Json
