#pragma once

// Test-only. Domain types in CardCatalog.h/Decks.h/Games.h/Identifiers.h
// expose no public toJson()/QJsonObject/QJsonArray/QJsonValue-returning
// encoder of their own (see tests/EncoderHygieneTests.cpp's declaration-
// absence policy, which enforces this at the src/ header level): every
// caller -- production or test -- composes a domain value's own
// toRawJson() (the lossless Json::Value AST; see RawJson.h) with the
// single centralized, bounded, fallible Value::toExactQJson()/
// toExactQJsonObject()/toExactQJsonArray() adapter instead. These
// templates exist purely to remove the two-line
// `auto raw = value.toRawJson(); if (!raw) return failure(raw.error());
// return raw->toExactQJsonObject();` boilerplate this composition would
// otherwise repeat at every one of this test suite's call sites, not to
// reintroduce a per-DTO wrapper: they are declared here, under tests/,
// specifically because the policy above scans src/**/*.h only.
//
// toRawJson() itself returns either a non-fallible Json::Value (e.g.
// CardCost, GameValue, TypedId<Tag>) or a fallible
// ValueOrError<Json::Value> (e.g. SkillIcon, CardDef, most of
// Decks.h/Games.h), depending on the type; assigning either result
// directly to a local ValueOrError<Json::Value> uses
// ValueOrError<T>::ValueOrError(T) for the non-fallible case and a plain
// copy/move for the already-fallible case, so a single template body
// below handles both shapes without any if constexpr branching.

#include "RawJson.h"
#include "ValueOrError.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace Arkham::TestOnly {

// For a domain type whose toJson() used to return
// ValueOrError<QJsonObject>/QJsonObject.
template <typename T>
[[nodiscard]] ValueOrError<QJsonObject> objectJson(const T &value) {
  ValueOrError<Json::Value> raw = value.toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toExactQJsonObject();
}

// For a domain type/collection whose toJson() used to return
// ValueOrError<QJsonArray>/QJsonArray.
template <typename T>
[[nodiscard]] ValueOrError<QJsonArray> arrayJson(const T &value) {
  ValueOrError<Json::Value> raw = value.toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toExactQJsonArray();
}

// For a fragment type whose toJson() used to return
// ValueOrError<QJsonValue>/QJsonValue (e.g. CardCode, NonEmptyString<Tag>,
// ExternalDeckId).
template <typename T>
[[nodiscard]] ValueOrError<QJsonValue> scalarJson(const T &value) {
  ValueOrError<Json::Value> raw = value.toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toExactQJson();
}

// Overload for a free function's already-produced ValueOrError<Json::Value>
// (e.g. encodeGameListToRawJson(rows)), rather than a single value's own
// member toRawJson().
[[nodiscard]] inline ValueOrError<QJsonArray>
arrayJsonFromRaw(const ValueOrError<Json::Value> &raw) {
  if (!raw)
    return failure(raw.error());
  return raw->toExactQJsonArray();
}

} // namespace Arkham::TestOnly
