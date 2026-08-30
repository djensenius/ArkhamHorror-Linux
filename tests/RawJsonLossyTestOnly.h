#pragma once

// Defines Arkham::Json::rawValueToLossyQJsonForTestingOnly(), the ONLY
// definition of the lossy Value -> QJsonValue conversion this test suite
// asserts against (RawJsonTests.cpp). This header is intentionally NOT
// part of the arkham_foundation production target (see CMakeLists.txt:
// it lives under tests/ and is only ever #included by
// tests/RawJsonTests.cpp) -- production/domain code has no include path
// to it at all, which is the point.
//
// History: earlier revisions kept this exact logic as a *private*
// Value::toLossyQJsonForTestingOnly() member function directly inside
// src/RawJson.h/.cpp, reachable only via a `friend class ::RawJsonTests;`
// grant. A cumulative review correctly flagged that this still left a
// hidden lossy escape hatch physically present in the production header
// -- one `private:` -> `public:` access-specifier edit away from being
// callable from production code again -- and that a scanner exempting
// RawJson.h wholesale by basename could never notice such a flip.
// Relocating the actual conversion logic here removes that risk
// structurally: RawJson.h's class Value now grants friendship via a
// single "hidden friend" declaration (`friend QJsonValue
// rawValueToLossyQJsonForTestingOnly(const Value &value);`, declared
// UNQUALIFIED inside the class body, with no separate forward
// declaration anywhere else in that header) rather than a whole-class
// `friend class ::RawJsonTests;` grant, so there is no access specifier
// left in the production header to flip, and the actual conversion body
// is defined only here, out-of-line, matching that friend declaration --
// compiled into zero production translation units. The function must be
// defined directly in namespace Arkham::Json (NOT a nested sub-
// namespace) to match where the hidden friend is injected; see the
// friend declaration's own doc comment in RawJson.h for the full
// mechanics.
//
// This is deliberately WITHOUT any of Value::toExactQJson()'s invariant
// checks: lossless for every Kind except a Number whose literal is not
// RawNumber::toExactInt64() (i.e. a genuine decimal, or an integral
// value outside qint64's range), which round-trips only as closely as
// IEEE-754 double allows, and silently collapses a duplicate object key
// / drops a nested Kind::Undefined member rather than rejecting either.
// This is the exact lossy behavior every production encoder in this
// codebase must NOT exhibit (see the cumulative-review history in
// CardCatalog.cpp/Games.cpp/Decks.cpp -- every public toJson()/
// toQJson()-shaped response or request encoder was rewritten to build a
// complete Json::Value AST and convert it exactly once via
// toExactQJson()/toExactQJsonObject()/toExactQJsonArray() instead of
// ever calling anything like this). It exists here ONLY so
// RawJsonTests.cpp can assert this documented lossy fallback behavior
// still exists in the underlying AST type itself (i.e. that
// toExactQJson() is a deliberate, tested divergence from it, not an
// accidental one).

#include "RawJson.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace Arkham::Json {

inline QJsonValue rawValueToLossyQJsonForTestingOnly(const Value &value) {
  switch (value.m_kind) {
  case Value::Kind::Undefined:
    return QJsonValue(QJsonValue::Undefined);
  case Value::Kind::Null:
    return QJsonValue(QJsonValue::Null);
  case Value::Kind::Bool:
    return QJsonValue(value.m_bool);
  case Value::Kind::Number:
    // Preserve full int64 precision whenever the literal is mathematically
    // integral and in range (see RawNumber::toExactInt64()): Qt's
    // QCborValue-backed QJsonValue(qint64) constructor stores such a value
    // exactly (QJsonValue::toInteger() on the result returns the identical
    // qint64, unlike a value built via the double constructor -- verified
    // against Qt 6.11's QJsonValue/QCborValue implementation). Every other
    // literal (a genuine decimal, or an integral value outside qint64's
    // range) still round-trips only as closely as IEEE-754 double allows.
    if (auto exact = value.m_number.toExactInt64())
      return QJsonValue(*exact);
    return QJsonValue(value.m_number.toDouble());
  case Value::Kind::String:
    return QJsonValue(value.m_string);
  case Value::Kind::Array: {
    QJsonArray array;
    for (const auto &element : value.m_array)
      array.append(rawValueToLossyQJsonForTestingOnly(element));
    return array;
  }
  case Value::Kind::Object: {
    QJsonObject object;
    for (const auto &[k, v] : value.m_object)
      object.insert(k, rawValueToLossyQJsonForTestingOnly(v));
    return object;
  }
  }
  return QJsonValue(QJsonValue::Undefined);
}

} // namespace Arkham::Json
