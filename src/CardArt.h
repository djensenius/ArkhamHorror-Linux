#pragma once

#include "Board.h"

#include <QString>

// Maps decoded board entities (Board.h) to `image://arkham-card/<id>` image
// sources for the AssetCardImageProvider (see
// AssetCardImageProvider::parseImageId() for the id grammar). These are the
// web client's own art rules, kept as small pure functions:
//
// - Investigator (Investigator.vue): its `art`, plus `_<mutated>` when the
//   art is mutated, e.g. "c01001?mutation=Mutated21" for
//   /img/arkham/cards/01001_Mutated21.avif.
// - Location (Location.vue): the back while unrevealed, except an
//   enemy-location, which always shows its front.
// - Agenda (Agenda.vue): the back when flipped. The trailing 'a' of the id
//   is dropped first, except for c03276a and c03279a, whose own back is
//   "<id>b".
// - Act: the front only.
//
// Not covered yet (follow-ups): Act.vue's per-side art for flipped or
// multi-sided acts (sequence sides B..H, c10607a), the c03006/c90087
// class-variant suffix that depends on investigator `meta`, and the
// investigator `form` overrides (Shattered/Transfigured).
namespace Arkham::CardArt {

// The provider id ("<code>", "back/<code>", "<code>?mutation=MutatedN").
[[nodiscard]] QString investigatorImageId(const Investigator &investigator);
[[nodiscard]] QString locationImageId(const Location &location);
[[nodiscard]] QString agendaImageId(const Agenda &agenda);
[[nodiscard]] QString actImageId(const Act &act);

// "image://arkham-card/" + imageId, ready for an Image's `source`.
[[nodiscard]] QString imageSource(const QString &imageId);

} // namespace Arkham::CardArt
