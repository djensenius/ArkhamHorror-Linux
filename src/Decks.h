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
  // `value` must have come from Json::Value::parse(),
  // Json::RawNumber::fromInt64(), or be default-constructed: all three
  // guarantee a valid, non-empty digit string. A default-constructed
  // Json::RawNumber is specifically the canonical "0" literal (see its
  // default constructor in RawJson.h), not an unrepresentable
  // empty-digit state, so it round-trips through
  // Json::Value::toJsonBytesInner() the same as any parsed or
  // fromInt64()-produced value.
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

  // The lossless Json::Value AST fragment for this id, for splicing into
  // a request built via Json::Value's builder statics (see RawJson.h). No
  // public toJson()/QJsonValue-returning encoder is exposed here; every
  // caller composes this raw AST with the single central
  // Value::toExactQJson() adapter (see RawJson.h) instead --
  // DeckListInput::toJsonBytes() is the canonical production path.
  // Precondition: kind() != Kind::Absent -- an absent id has no JSON
  // representation at all (the caller must omit the "id" key from the
  // enclosing object entirely, not insert some sentinel value); calling
  // this for Kind::Absent returns a default-constructed Json::Value
  // (Kind::Undefined) defensively (never crashes), never
  // Json::Value::makeNull(). Relying on that is still a caller bug: every
  // outbound request encoder in this codebase composes this fragment
  // into an enclosing Json::Value object and serializes it via the
  // canonical Json::Value::toJsonBytes()/toExactQJson() (see RawJson.h/
  // .cpp), and both reject a nested Kind::Undefined member with a typed
  // failure rather than silently dropping the key -- so forgetting this
  // precondition fails loudly at encode time, it does not silently omit
  // the field. (Only a separate, non-canonical, test-only lossy
  // conversion -- see tests/RawJsonTests.cpp -- would silently drop
  // such a member, matching QJsonObject::insert()'s own documented behavior for
  // QJsonValue::Undefined; no outbound request encoder uses that path.)
  [[nodiscard]] Json::Value toRawJson() const;

  friend bool operator==(const ExternalDeckId &,
                         const ExternalDeckId &) = default;

  // Explicitly declared (rather than left to the compiler's implicit
  // move constructor/assignment) specifically to suppress move: m_kind
  // is a plain enum a compiler-generated move leaves completely
  // unchanged on the moved-from source, while m_text is a QString whose
  // real move constructor/assignment DOES leave the moved-from source
  // empty. Left implicit, a moved-from Kind::Text ExternalDeckId would
  // still report kind() == Kind::Text but text() would be empty --
  // exactly the "populated only when kind() == Kind::Text" invariant
  // this class's accessors document, silently violated. A user-declared
  // copy constructor/assignment here means there is no user-declared
  // move constructor/assignment for the compiler to implicitly
  // generate, so std::move(id) instead binds to this copy constructor
  // (an rvalue can bind to `const ExternalDeckId&`), leaving the
  // moved-from source completely unchanged -- QString's copy
  // constructor is noexcept and O(1) (implicit sharing), and
  // Json::RawNumber is already copy-only for the identical reason (see
  // its own doc comment), so this costs nothing relative to a "real"
  // move while making a moved-from ExternalDeckId structurally
  // impossible to observe as invalid, including through a container
  // that relocates/moves its elements.
  ExternalDeckId(const ExternalDeckId &) = default;
  ExternalDeckId &operator=(const ExternalDeckId &) = default;

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
  // Canonical byte-level decode: identical logic to fromJson() above
  // (shared via a private template, see Decks.cpp), operating directly on
  // the lossless AST (see RawJson.h) parsed by Json::Value::parse() --
  // never converting the whole tree to QJsonValue first -- so `id`'s
  // numeric variant and any number nested inside `sideSlots` (at any
  // depth) survive a decode byte-exact, including a number outside
  // qint64/double range, a long fraction, or a huge exponent. A duplicate
  // object key anywhere in the payload is rejected by Json::Value::parse()
  // itself (see RawJson.cpp), never silently collapsed. The one production
  // entry point governed fixtures and any future
  // request/response body this client decodes must use.
  [[nodiscard]] static ValueOrError<DeckListInput>
  fromRawJson(const Json::Value &v, QStringView path);
  // Parses `bytes` per RFC 8259 exactly (see RawJson.h's Value::parse())
  // and decodes via fromRawJson() above -- never collapsing to QJsonValue
  // first, unlike this method's previous "decode via fromJson(), then
  // patch id/sideSlots back in from the raw tree" implementation, which
  // could not preserve a number nested anywhere else callers might add.
  [[nodiscard]] static ValueOrError<DeckListInput>
  fromRawBytes(QByteArrayView bytes, QStringView path);
  // The lossless Json::Value AST fragment for this request body (see
  // RawJson.h); used by toJsonBytes() below and by any enclosing request
  // (e.g. CreateDeckRequest, ChooseDeckRequest) that needs to splice a
  // whole DeckListInput into a larger AST without a lossy
  // encode-then-reparse round trip. No public toJson()/QJsonObject-
  // returning encoder is exposed here; every caller composes this raw
  // AST with the single central Value::toExactQJsonObject() adapter (see
  // RawJson.h), or uses toJsonBytes() below for outbound request bytes.
  // Fails (rather than emitting a schema-invalid request) if `cardSlots`
  // -- a public QMap<QString, qint64> field, for ergonomic construction
  // from a permissive external caller -- carries an empty key: unlike
  // decode (which never produces one; see decodeCardQuantityMapInput),
  // an encoder must still guard a hand-constructed instance against
  // this.
  [[nodiscard]] ValueOrError<Json::Value> toRawJson() const;
  // Precision-preserving canonical encoder: builds the complete request
  // as a lossless Json::Value AST (see RawJson.h) and serializes it once,
  // so `id`'s numeric variant and any number nested inside `sideSlots`
  // are encoded byte-exact rather than re-encoded through a double.
  // Returns a typed failure rather than ever falling back to a lossy
  // encoding (e.g. if `id`/`sideSlots` somehow could not be serialized --
  // see Json::Value::toJsonBytes()'s own failure cases).
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
  // Canonical byte-level decode overload: identical logic (shared via a
  // private template, see Decks.cpp), operating directly on the lossless
  // AST (see RawJson.h) so a Deck::fromRawJson()/fromRawBytes() caller
  // stays on Json::Value end-to-end for this nested field.
  [[nodiscard]] static ValueOrError<DeckList> fromRawJson(const Json::Value &v,
                                                          QStringView path);
  // Parses `bytes` through the canonical raw-byte parser (see RawJson.h)
  // and decodes via fromRawJson() above.
  [[nodiscard]] static ValueOrError<DeckList> fromRawBytes(QByteArrayView bytes,
                                                           QStringView path);
  // The lossless Json::Value AST for this response DTO (see RawJson.h);
  // used by Deck::toRawJson() below to compose a whole Deck's AST
  // without a lossy encode-then-reparse round trip. No public toJson()/
  // QJsonObject-returning encoder is exposed here; every caller composes
  // this raw AST with the single central Value::toExactQJsonObject()
  // adapter (see RawJson.h) instead. Never fails: every field here is
  // either already-validated (investigatorCode/cardSlots/sideSlots) or a
  // plain optional/required QString/qint64 with no construction-time
  // invariant of its own -- the central adapter's exact conversion is
  // what actually enforces string length/lone-surrogate/duplicate-key
  // bounds.
  [[nodiscard]] Json::Value toRawJson() const;

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
  // Canonical byte-level decode overload: identical logic (shared via a
  // private template, see Decks.cpp), operating directly on the lossless
  // AST (see RawJson.h) so this deck's list.cardSlots/sideSlots quantities
  // (and any future unconstrained field) survive undamaged end-to-end.
  [[nodiscard]] static ValueOrError<Deck> fromRawJson(const Json::Value &v,
                                                      QStringView path);
  // Parses `bytes` through the canonical raw-byte parser (see RawJson.h)
  // and decodes via fromRawJson() above.
  [[nodiscard]] static ValueOrError<Deck> fromRawBytes(QByteArrayView bytes,
                                                       QStringView path);
  // The lossless Json::Value AST for this response DTO (see RawJson.h);
  // composes list.toRawJson() above directly rather than via any
  // QJsonObject/QJsonValue intermediary. No public toJson()/QJsonObject-
  // returning encoder is exposed here; every caller composes this raw
  // AST with the single central Value::toExactQJsonObject() adapter (see
  // RawJson.h) instead.
  [[nodiscard]] Json::Value toRawJson() const;

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
  // Canonical byte-level decode: see DeckListInput::fromRawJson()'s doc
  // comment -- decodes deckList natively via the same lossless AST rather
  // than reparsing it a second time from re-serialized bytes.
  [[nodiscard]] static ValueOrError<CreateDeckRequest>
  fromRawJson(const Json::Value &v, QStringView path);
  // Parses `bytes` per RFC 8259 exactly and decodes via fromRawJson()
  // above; see DeckListInput::fromRawBytes().
  [[nodiscard]] static ValueOrError<CreateDeckRequest>
  fromRawBytes(QByteArrayView bytes, QStringView path);
  // The lossless Json::Value AST fragment for this request body (see
  // RawJson.h); used by toJsonBytes() below to encode
  // deckList/deckId/deckName/deckUrl. No public toJson()/QJsonObject-
  // returning encoder is exposed here; every caller composes this raw
  // AST with the single central Value::toExactQJsonObject() adapter (see
  // RawJson.h) instead.
  [[nodiscard]] ValueOrError<Json::Value> toRawJson() const;
  // Precision-preserving canonical encoder; see
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
  // Canonical byte-level decode: identical logic to fromJson() above
  // (shared via a template, see Decks.cpp). fetchDeckRequest's own
  // additionalProperties is explicitly `true` in decks.schema.json, unlike
  // deckList/deck/deckValidationError/deckOperationError, so this
  // deliberately does NOT enforce an exact key set here.
  [[nodiscard]] static ValueOrError<FetchDeckRequest>
  fromRawJson(const Json::Value &v, QStringView path);
  // Parses `bytes` per RFC 8259 exactly and decodes via fromRawJson()
  // above.
  [[nodiscard]] static ValueOrError<FetchDeckRequest>
  fromRawBytes(QByteArrayView bytes, QStringView path);
  // Canonical byte-level encode (round-10-cumulative-review item 2):
  // composes a Json::Value AST directly and serializes it via
  // Value::toJsonBytes(), which rejects a lone/mismatched UTF-16
  // surrogate in `url` with a typed failure rather than ever emitting
  // invalid UTF-8 (see RawJson.h's appendJsonEncodedString()). No public
  // toJson()/QJsonObject-returning encoder is exposed here; every caller
  // composes this raw AST with the single central
  // Value::toExactQJsonObject() adapter (see RawJson.h) instead.
  [[nodiscard]] ValueOrError<Json::Value> toRawJson() const;
  [[nodiscard]] ValueOrError<QByteArray> toJsonBytes() const;

  friend bool operator==(const FetchDeckRequest &,
                         const FetchDeckRequest &) = default;
};

// A single `slots` validation failure: a card code the backend does not
// implement.
struct DeckValidationError {
  CardCode cardCode;

  [[nodiscard]] static ValueOrError<DeckValidationError>
  fromJson(const QJsonValue &v, QStringView path);
  // Canonical byte-level decode: identical logic to fromJson() above
  // (shared via a template, see Decks.cpp), operating directly on the
  // lossless AST (see RawJson.h) and enforcing decks.schema.json's
  // deckValidationError's exact {"tag","contents"} shape
  // (additionalProperties:false) rather than accepting-and-discarding an
  // extra key (round-10-cumulative-review item 5).
  [[nodiscard]] static ValueOrError<DeckValidationError>
  fromRawJson(const Json::Value &v, QStringView path);
  // Parses `bytes` through the canonical raw-byte parser (see RawJson.h),
  // rejecting a duplicate object key (including an escape-equivalent one)
  // before any nested decode runs, and decodes via fromRawJson() above.
  [[nodiscard]] static ValueOrError<DeckValidationError>
  fromRawBytes(QByteArrayView bytes, QStringView path);
  // The lossless Json::Value AST for this response DTO (see RawJson.h);
  // used by DeckValidationResult::toRawJson() below to compose the whole
  // result array's AST without a lossy encode-then-reparse round trip.
  // No public toJson()/QJsonObject-returning encoder is exposed here;
  // every caller composes this raw AST with the single central
  // Value::toExactQJsonObject() adapter (see RawJson.h) instead. Never
  // fails: cardCode is already validated at construction and "tag" is a
  // fixed literal.
  [[nodiscard]] Json::Value toRawJson() const;

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
  // Canonical byte-level decode: each element decodes through
  // DeckValidationError::fromRawJson() (see its doc comment), so a
  // duplicate/extra key nested inside any one entry is caught before this
  // client ever collapses the whole array to QJsonValue first.
  [[nodiscard]] static ValueOrError<DeckValidationResult>
  fromRawJson(const Json::Value &v, QStringView path);
  // Parses `bytes` through the canonical raw-byte parser (see RawJson.h)
  // and decodes via fromRawJson() above.
  [[nodiscard]] static ValueOrError<DeckValidationResult>
  fromRawBytes(QByteArrayView bytes, QStringView path);
  // The lossless Json::Value AST for this response DTO (see RawJson.h).
  // No public toJson()/QJsonArray-returning encoder is exposed here;
  // every caller composes this raw AST with the single central
  // Value::toExactQJsonArray() adapter (see RawJson.h) instead. Never
  // fails: each element's own toRawJson() is itself infallible.
  [[nodiscard]] Json::Value toRawJson() const;

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

  // Explicitly declared (rather than left to the compiler's implicit
  // move constructor/assignment) specifically to suppress move: m_kind
  // is a plain enum a compiler-generated move leaves completely
  // unchanged on the moved-from source, while m_errors is a
  // QList<DeckValidationError> whose real move constructor/assignment
  // DOES leave the moved-from source empty. Left implicit, a moved-from
  // Kind::Errors instance would still report kind() == Kind::Errors but
  // errorList() would be empty -- silently masquerading as
  // deckValidationSuccess (the empty array) to any caller that then
  // calls toRawJson() on the moved-from source, exactly the "Errors ->
  // Success" collapse this class's own class-level doc comment
  // describes as structurally impossible. A user-declared copy
  // constructor/assignment here means there is no user-declared move
  // constructor/assignment for the compiler to implicitly generate, so
  // std::move(result) instead binds to this copy constructor (an
  // rvalue can bind to `const DeckValidationResult&`), leaving the
  // moved-from source completely unchanged -- QList's copy constructor
  // is noexcept and O(1) (implicit sharing), so this costs nothing
  // relative to a "real" move while making that collapse structurally
  // impossible to observe, including through a container that
  // relocates/moves its elements.
  DeckValidationResult(const DeckValidationResult &) = default;
  DeckValidationResult &operator=(const DeckValidationResult &) = default;

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
  // Canonical byte-level decode: enforces decks.schema.json's
  // deckOperationError's exact {"errorMsg"} shape
  // (additionalProperties:false) rather than accepting-and-discarding an
  // extra key (round-10-cumulative-review item 5).
  [[nodiscard]] static ValueOrError<DeckOperationError>
  fromRawJson(const Json::Value &v, QStringView path);
  // Parses `bytes` through the canonical raw-byte parser (see RawJson.h),
  // rejecting a duplicate object key before any nested decode runs, and
  // decodes via fromRawJson() above.
  [[nodiscard]] static ValueOrError<DeckOperationError>
  fromRawBytes(QByteArrayView bytes, QStringView path);
  // The lossless Json::Value AST for this response DTO (see RawJson.h).
  // No public toJson()/QJsonObject-returning encoder is exposed here;
  // every caller composes this raw AST with the single central
  // Value::toExactQJsonObject() adapter (see RawJson.h) instead --
  // errorMsg is validated the same bounded/lone-surrogate-safe way there
  // rather than inserted into a QJsonObject directly, since a backend
  // response's errorMsg is decoded, unvalidated-for-encodability QString
  // content that could otherwise re-serialize silently as an
  // unencodable QJsonObject.
  [[nodiscard]] Json::Value toRawJson() const;

  friend bool operator==(const DeckOperationError &,
                         const DeckOperationError &) = default;
};

} // namespace Arkham
