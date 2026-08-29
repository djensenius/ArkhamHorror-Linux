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
enum class ExternalDeckIdTag { Absent, Null, Text, Number };
struct ExternalDeckId {
  ExternalDeckIdTag tag{ExternalDeckIdTag::Absent};
  // Populated only when tag == Text.
  QString text;
  // Populated only when tag == Number: the number's JSON literal text
  // (sign/digits/fraction/exponent), reconstructed via
  // Json::RawNumber::literal()/Json::scientificShow() without ever
  // rounding through a double, so arbitrary-precision integers past
  // double's exact-integer range (2^53) -- which ArkhamDB is known to hand
  // out as deck ids -- round-trip exactly. This is precision-preserving,
  // not necessarily byte-for-byte: RawNumber::literal() always spells the
  // exponent marker lowercase ('e') and omits a redundant leading '+' on a
  // positive exponent, so a source literal spelled e.g. "1E+5" is
  // canonicalized to "1e5" here (same numeric value, different spelling).
  // fromObject()/toJson() below -- which must operate on an
  // already-parsed QJsonValue/QJsonObject and therefore cannot recover
  // precision QJsonDocument has already destroyed -- are only a
  // best-effort fallback; fromRawBytes()/toJsonBytes() are this type's
  // genuinely lossless (up to the canonicalization above) entry points
  // and should be preferred by any caller that has the original bytes.
  QString numberLiteral;

  // Reads the "id" key directly from obj (needs the whole object, not just
  // a value, to distinguish an absent key from an explicit null). Lossy
  // for tag == Number: see numberLiteral's doc comment.
  [[nodiscard]] static ValueOrError<ExternalDeckId>
  fromObject(const QJsonObject &obj, QStringView path);
  // Precision-preserving equivalent of fromObject(), reading "id" from a
  // Json::Value object already produced by the canonical raw-byte parser
  // (see RawJson.h) instead of QJsonObject.
  [[nodiscard]] static ValueOrError<ExternalDeckId>
  fromRawObject(const Json::Value &obj, QStringView path);
  // Undefined when tag == Absent, so callers can omit the key entirely; a
  // real QJsonValue::Null/String/Double otherwise. Lossy for tag ==
  // Number: see numberLiteral's doc comment; toJsonLiteral() below is
  // the lossless equivalent for that case.
  [[nodiscard]] QJsonValue toJson() const;
  // The exact JSON literal to splice into a byte-level encode for this id
  // (used by DeckListInput::toJsonBytes()); only meaningful for tag ==
  // Number, where it returns numberLiteral (see that field's doc comment
  // on the precision-vs-verbatim distinction).
  [[nodiscard]] const QString &toJsonLiteral() const { return numberLiteral; }

  friend bool operator==(const ExternalDeckId &,
                         const ExternalDeckId &) = default;
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
  // codes.
  QMap<QString, int> cardSlots;
  // Schema-unconstrained and possibly malformed by design (the fixture
  // itself supplies a legacy array here); kept as raw JSON exactly as
  // received so a malformed or absent value can never be mistaken for an
  // already-normalized empty map. Undefined when the key was absent.
  QJsonValue sideSlots{QJsonValue::Undefined};
  // Required. Permissive: may or may not carry the CardCode "c" prefix.
  InvestigatorRef investigatorCode;
  std::optional<QString> investigatorName;
  std::optional<QString> meta;
  std::optional<int> tabooId;
  std::optional<QString> url;
  ExternalDeckId id;
  std::optional<QString> name;

  [[nodiscard]] static ValueOrError<DeckListInput> fromJson(const QJsonValue &v,
                                                            QStringView path);
  // Precision-preserving equivalent of fromJson(): every field decodes
  // identically, except `id`'s numeric variant keeps its exact source
  // literal (see ExternalDeckId::numberLiteral) instead of being rounded
  // through a double by QJsonDocument before this type ever sees it.
  // Callers with the original request/response bytes should prefer this
  // over fromJson().
  [[nodiscard]] static ValueOrError<DeckListInput>
  fromRawBytes(QByteArrayView bytes, QStringView path);
  [[nodiscard]] QJsonObject toJson() const;
  // Precision-preserving equivalent of toJson(): identical bytes except
  // `id`'s numeric variant is spliced in from its canonicalized source
  // literal (see ExternalDeckId::numberLiteral) rather than re-encoded
  // through a double.
  [[nodiscard]] QByteArray toJsonBytes() const;

  friend bool operator==(const DeckListInput &,
                         const DeckListInput &) = default;
};

// The backend-normalized deck-list shape: every one of decks.schema.json's
// nine `deckList` fields is always present, so decode requires (rather than
// merely permits) each key while still allowing several to carry an
// explicit JSON null value.
struct DeckList {
  QMap<CardCode, int> cardSlots;
  QMap<CardCode, int> sideSlots;
  CardCode investigatorCode;
  QString investigatorName;
  std::optional<QString> meta;
  std::optional<int> tabooId;
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
  int userId{0};
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
  [[nodiscard]] QByteArray toJsonBytes() const;

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
