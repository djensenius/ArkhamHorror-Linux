#pragma once

#include "Identifiers.h"
#include "RawJson.h"
#include "ValueOrError.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>
#include <optional>
#include <utility>

namespace Arkham {

// decks.schema.json's `deckListInput.id` may be absent, explicit JSON null,
// a string, or a number -- and per Arkham.Decklist.Type's hand-written
// parser, absent and explicit-null decode to genuinely different results
// downstream (absent stays unset; explicit null normalizes to an empty
// string), so this type keeps all four states distinguishable rather than
// collapsing absent/null the way the optional* JsonDecode helpers do.
//
// Private-constructor sum type (mirrors CardCost/GameValue/SkillIcon's
// convention elsewhere in this codebase): the Number variant can only be
// produced from a Json::RawNumber, which -- unlike a plain QString/double
// -- can only itself be constructed by Json::Value::parse() or
// Json::RawNumber::fromInt64(), never by splicing arbitrary caller text.
// This makes the historical defect this type previously had (a public,
// independently-mutable `numberLiteral` QString field that toJson()
// encoded by rounding through a double, and whose validation failure
// silently fell back to that same lossy 0.0-on-failure path) structurally
// unrepresentable rather than merely tested against: there is no public
// mutator, and no code path -- valid or otherwise -- ever encodes a
// Number variant by parsing/round-tripping arbitrary text through a
// double.
class ExternalDeckId {
public:
  enum class Kind { Absent, Null, Text, Number };

  // Kind::Absent, matching an omitted "id" key.
  ExternalDeckId() = default;

  [[nodiscard]] static ExternalDeckId absent();
  [[nodiscard]] static ExternalDeckId null();
  [[nodiscard]] static ExternalDeckId text(QString value);
  // `value` must have come from Json::Value::parse() or
  // Json::RawNumber::fromInt64() (both guarantee a valid, non-empty digit
  // string); a default-constructed Json::RawNumber is rejected by
  // Json::Value::toJsonBytesInner() defensively but should never be
  // passed here in the first place.
  [[nodiscard]] static ExternalDeckId number(Json::RawNumber value);

  [[nodiscard]] Kind kind() const noexcept { return m_kind; }
  // Populated only when kind() == Kind::Text.
  [[nodiscard]] const QString &text() const noexcept { return m_text; }
  // Populated only when kind() == Kind::Number.
  [[nodiscard]] const Json::RawNumber &number() const noexcept {
    return m_number;
  }

  // Reads the "id" key directly from obj (needs the whole object, not just
  // a value, to distinguish an absent key from an explicit null). A
  // Kind::Number result is only as precise as the source QJsonValue --
  // exact for any qint64-range integer (QJsonValue's underlying
  // QCborValue preserves int64 precision even though isDouble()/
  // toDouble() report it as a rounded double; see toInteger()), best-
  // effort (IEEE-754 double) otherwise. fromRawObject() below is
  // preferred when the original bytes are available, since it can also
  // preserve a genuinely fractional/huge-exponent literal exactly.
  [[nodiscard]] static ValueOrError<ExternalDeckId>
  fromObject(const QJsonObject &obj, QStringView path);
  // Precision-preserving equivalent of fromObject(), reading "id" from a
  // Json::Value object already produced by the canonical raw-byte parser
  // (see RawJson.h) instead of QJsonObject.
  [[nodiscard]] static ValueOrError<ExternalDeckId>
  fromRawObject(const Json::Value &obj, QStringView path);

  // Lossy, display/log/debug-only conversion: Kind::Number rounds through
  // a double exactly like the rest of this codebase's non-byte QJsonValue
  // decoders. NEVER use this to build outbound request bytes -- see
  // toRawJson() for the lossless equivalent DeckListInput::toJsonBytes()
  // actually uses.
  [[nodiscard]] QJsonValue toJson() const;
  // The lossless Json::Value AST fragment for this id, for splicing into
  // a request built via Json::Value's builder statics (see RawJson.h).
  // Precondition: kind() != Kind::Absent -- an absent id has no JSON
  // representation at all (the caller must omit the "id" key from the
  // enclosing object entirely, not insert some sentinel value); calling
  // this for Kind::Absent returns Json::Value::makeNull() defensively
  // (never crashes) but is a caller bug, since it would wrongly encode an
  // omitted id as an explicit null.
  [[nodiscard]] Json::Value toRawJson() const;

  friend bool operator==(const ExternalDeckId &,
                         const ExternalDeckId &) = default;

private:
  Kind m_kind{Kind::Absent};
  QString m_text;
  Json::RawNumber m_number;
};

// The permissive, externally-supplied deck-list shape accepted by
// createDeck.deckList and POST /decks/validate. Preserves the caller's slot
// keys, investigator code, and sideSlots verbatim (no normalization -- see
// contracts/README.md: normalization is a backend-only concern this client
// only ever decodes the *result* of, never reconstructs). Unknown extra
// fields (e.g. legacy ArkhamDB export fields) are accepted and ignored, per
// the schema's implicit `additionalProperties: true`.
struct DeckListInput {
  // Required. Keys need only be non-empty (schema: cardQuantityMapInput);
  // unlike the normalized DeckList, they are not required to look like card
  // codes. Values are exact qint64 (see JsonDecode.h::requireIntValue) --
  // never rounded through a 32-bit int.
  QMap<QString, qint64> cardSlots;
  // Schema-unconstrained and possibly malformed by design (the fixture
  // itself supplies a legacy array here); kept as a lossless Json::Value
  // tree exactly as received (see RawJson.h) rather than a QJsonValue, so
  // a malformed or absent value can never be mistaken for an
  // already-normalized empty map, AND any number nested inside it --
  // however deep -- survives byte-exact through toJsonBytes() rather than
  // being rounded through a double. Kind::Undefined when the key was
  // absent. Populated via Json::Value::fromQJson() when decoded through
  // fromJson() (best-effort: as precise as the source QJsonValue already
  // is) or read directly from the raw parse tree when decoded through
  // fromRawBytes() (exact).
  Json::Value sideSlots;
  // Required. Permissive: may or may not carry the CardCode "c" prefix.
  InvestigatorRef investigatorCode;
  std::optional<QString> investigatorName;
  std::optional<QString> meta;
  std::optional<qint64> tabooId;
  std::optional<QString> url;
  ExternalDeckId id;
  std::optional<QString> name;

  [[nodiscard]] static ValueOrError<DeckListInput> fromJson(const QJsonValue &v,
                                                            QStringView path);
  // Precision-preserving equivalent of fromJson(): every field decodes
  // identically, except `id`'s numeric variant and `sideSlots` (including
  // any number nested inside it) keep their exact source literal(s)
  // instead of being rounded through a double by QJsonDocument before
  // this type ever sees them. Callers with the original request/response
  // bytes should prefer this over fromJson().
  [[nodiscard]] static ValueOrError<DeckListInput>
  fromRawBytes(QByteArrayView bytes, QStringView path);
  [[nodiscard]] QJsonObject toJson() const;
  // The lossless Json::Value AST fragment for this request body (see
  // RawJson.h); used by toJsonBytes() below and by any enclosing request
  // (e.g. CreateDeckRequest, ChooseDeckRequest) that needs to splice a
  // whole DeckListInput into a larger AST without a lossy
  // encode-then-reparse round trip.
  [[nodiscard]] Json::Value toRawJson() const;
  // Precision-preserving equivalent of toJson(): builds the complete
  // request as a lossless Json::Value AST (see RawJson.h) and serializes
  // it once, so `id`'s numeric variant and any number nested inside
  // `sideSlots` are encoded byte-exact rather than re-encoded through a
  // double. Returns a typed failure rather than ever falling back to a
  // lossy encoding (e.g. if `id`/`sideSlots` somehow could not be
  // serialized -- see Json::Value::toJsonBytes()'s own failure cases).
  [[nodiscard]] ValueOrError<QByteArray> toJsonBytes() const;

  friend bool operator==(const DeckListInput &,
                         const DeckListInput &) = default;
};

// The backend-normalized deck-list shape: every one of decks.schema.json's
// nine `deckList` fields is always present, so decode requires (rather than
// merely permits) each key while still allowing several to carry an
// explicit JSON null value.
struct DeckList {
  QMap<CardCode, qint64> cardSlots;
  QMap<CardCode, qint64> sideSlots;
  CardCode investigatorCode;
  QString investigatorName;
  std::optional<QString> meta;
  std::optional<qint64> tabooId;
  std::optional<QString> url;
  std::optional<QString> id;
  std::optional<QString> name;

  [[nodiscard]] static ValueOrError<DeckList> fromJson(const QJsonValue &v,
                                                       QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const DeckList &, const DeckList &) = default;
};

// A saved deck, as returned by the deck list/detail endpoints.
struct Deck {
  DeckId id;
  qint64 userId{0};
  std::optional<QString> url;
  QString name;
  QString investigatorName;
  DeckList list;

  [[nodiscard]] static ValueOrError<Deck> fromJson(const QJsonValue &v,
                                                   QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const Deck &, const Deck &) = default;
};

// POST /decks request body. `deckId` is required on the wire but ignored by
// the backend, which always returns its own database UUID instead (see
// contracts/README.md); it is still encoded so the request matches the
// schema.
struct CreateDeckRequest {
  QString deckId;
  QString deckName;
  std::optional<QString> deckUrl;
  DeckListInput deckList;

  [[nodiscard]] static ValueOrError<CreateDeckRequest>
  fromJson(const QJsonValue &v, QStringView path);
  // Precision-preserving equivalent of fromJson(); see
  // DeckListInput::fromRawBytes().
  [[nodiscard]] static ValueOrError<CreateDeckRequest>
  fromRawBytes(QByteArrayView bytes, QStringView path);
  [[nodiscard]] QJsonObject toJson() const;
  // Precision-preserving equivalent of toJson(); see
  // DeckListInput::toJsonBytes().
  [[nodiscard]] ValueOrError<QByteArray> toJsonBytes() const;

  friend bool operator==(const CreateDeckRequest &,
                         const CreateDeckRequest &) = default;
};

// POST /decks/fetch request body.
struct FetchDeckRequest {
  QString url;

  [[nodiscard]] static ValueOrError<FetchDeckRequest>
  fromJson(const QJsonValue &v, QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const FetchDeckRequest &,
                         const FetchDeckRequest &) = default;
};

// A single `slots` validation failure: a card code the backend does not
// implement.
struct DeckValidationError {
  CardCode cardCode;

  [[nodiscard]] static ValueOrError<DeckValidationError>
  fromJson(const QJsonValue &v, QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const DeckValidationError &,
                         const DeckValidationError &) = default;
};

// The result of validating a deck's slots: `deckValidationSuccess` (schema:
// `{"type": "array", "maxItems": 0}` -- exactly the empty array, never
// anything else) and `deckValidationErrors` (schema: `{"type": "array",
// "minItems": 1, ...}` -- one or more DeckValidationError entries) are two
// distinct, mutually exclusive schema shapes that happen to share the same
// element type, not one ambiguous "maybe empty" list. Modeling them as a
// single QList<DeckValidationError> would let an accidentally-empty
// "errors" value masquerade as success (or vice versa) purely by
// construction, with no way for the type system to catch it. This class
// makes that impossible: Kind::Errors can only be constructed via
// errors(), which itself fails for an empty list, and Kind::Success never
// carries any entries, so `kind() == Kind::Errors` and
// `!errorList().isEmpty()` are always equivalent -- there is no "empty
// errors" state to accidentally produce or observe.
class DeckValidationResult {
public:
  enum class Kind { Success, Errors };

  [[nodiscard]] static DeckValidationResult success();
  // Fails if `errors` is empty -- deckValidationErrors requires minItems:1,
  // so an empty error list is not a valid alternate spelling of success.
  [[nodiscard]] static ValueOrError<DeckValidationResult>
  errors(QList<DeckValidationError> errors);

  [[nodiscard]] static ValueOrError<DeckValidationResult>
  fromJson(const QJsonValue &v, QStringView path);
  [[nodiscard]] QJsonArray toJson() const;

  [[nodiscard]] Kind kind() const noexcept { return m_kind; }
  [[nodiscard]] bool isSuccess() const noexcept {
    return m_kind == Kind::Success;
  }
  // Empty for Kind::Success; guaranteed non-empty for Kind::Errors.
  [[nodiscard]] const QList<DeckValidationError> &errorList() const noexcept {
    return m_errors;
  }

  friend bool operator==(const DeckValidationResult &,
                         const DeckValidationResult &) = default;

private:
  DeckValidationResult() = default;

  Kind m_kind{Kind::Success};
  QList<DeckValidationError> m_errors;
};

// A generic deck-operation failure envelope (e.g. deck creation/sync).
struct DeckOperationError {
  QString errorMsg;

  [[nodiscard]] static ValueOrError<DeckOperationError>
  fromJson(const QJsonValue &v, QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const DeckOperationError &,
                         const DeckOperationError &) = default;
};

} // namespace Arkham
