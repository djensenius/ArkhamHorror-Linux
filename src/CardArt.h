#pragma once

#include "Board.h"

#include <QString>

// Maps decoded board entities (Board.h) to `image://arkham-card/<id>` image
// sources for the AssetCardImageProvider (see
// AssetCardImageProvider::parseImageId() for the id grammar). These are the
// web client's own art rules, kept as small pure functions:
//
// - Investigator (Investigator.vue), in the web's order: a form override
//   first (Yithian c04244, Homunculus c11068b, Shattered c10661,
//   Transfigured its card); otherwise its `art` (or its id when the art is
//   not a valid asset id), plus Lola Hayes' (c03006, c90087) role variant
//   and the mutation, e.g. "c03006?variant=Seeker&mutation=Mutated20" for
//   /img/arkham/cards/03006_Seeker_Mutated20.avif.
// - Location (Location.vue): the back while unrevealed, except an
//   enemy-location, which always shows its front.
// - Agenda (Agenda.vue): the back when flipped. The trailing 'a' of the id
//   is dropped first, except for c03276a and c03279a, whose own back is
//   "<id>b".
// - Act (Act.vue): the art for its sequence side, e.g. "c01108b" on side B,
//   "c10607b" for c10607a on side B, and the Threads of Fate ranges.
//
// Reprint and variant art (the web's reprintedArt/variantArt) is not
// covered.
namespace Arkham::CardArt {

// The provider id ("<code>", "back/<code>",
// "<code>?variant=<Class>&mutation=MutatedN").
[[nodiscard]] QString investigatorImageId(const Investigator &investigator);
[[nodiscard]] QString locationImageId(const Location &location);
[[nodiscard]] QString agendaImageId(const Agenda &agenda);
[[nodiscard]] QString actImageId(const Act &act);

// "image://arkham-card/" + imageId, ready for an Image's `source`.
[[nodiscard]] QString imageSource(const QString &imageId);

} // namespace Arkham::CardArt
