#pragma once

#include "Identifiers.h"
#include "ValueOrError.h"

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
  // Populated only when tag == Number. Stored as the double QJsonValue
  // already gave us; see JsonDecode.h's scientificShow() doc comment for why
  // Qt cannot preserve more precision than this for a JSON number.
  double number{0.0};

  // Reads the "id" key directly from obj (needs the whole object, not just
  // a value, to distinguish an absent key from an explicit null).
  [[nodiscard]] static ValueOrError<ExternalDeckId>
  fromObject(const QJsonObject &obj, QStringView path);
  // Undefined when tag == Absent, so callers can omit the key entirely; a
  // real QJsonValue::Null/String/Double otherwise.
  [[nodiscard]] QJsonValue toJson() const;

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
  [[nodiscard]] QJsonObject toJson() const;

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
  [[nodiscard]] QJsonObject toJson() const;

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

// Decodes a deck-validation result array: an empty array is
// deckValidationSuccess, a non-empty array is deckValidationErrors. Both
// share one wire shape (a JSON array of the same element schema), so one
// decode function covers both without a separate "success" type.
[[nodiscard]] ValueOrError<QList<DeckValidationError>>
decodeDeckValidationResult(const QJsonValue &v, QStringView path);
[[nodiscard]] QJsonArray
encodeDeckValidationResult(const QList<DeckValidationError> &errors);

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
