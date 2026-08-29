#include <array>
#include <utility>

#include <QSet>
#include <QtTest>

#include "ControllerGlyphs.h"

using Arkham::ControllerFamily;
using Arkham::ControllerGlyphs;
using Arkham::GamepadButton;

class ControllerGlyphTests final : public QObject {
  Q_OBJECT

private slots:
  void detectsFamilyFromDeviceNameHints();
  void unrecognizedOrEmptyHintsAreGeneric();
  void steamDeckIsCheckedBeforePlayStationAndXbox();
  void everyFamilyGivesADistinctGlyphForEveryButton();
  void glyphLookupIsDeterministicAcrossRepeatedCalls();
};

void ControllerGlyphTests::detectsFamilyFromDeviceNameHints() {
  const std::array<std::pair<QString, ControllerFamily>, 8> cases{{
      {QStringLiteral("Xbox Wireless Controller"), ControllerFamily::Xbox},
      {QStringLiteral("XBOX 360 Controller"), ControllerFamily::Xbox},
      {QStringLiteral("Sony Interactive Entertainment Wireless Controller"),
       ControllerFamily::PlayStation},
      {QStringLiteral("DualSense Wireless Controller"),
       ControllerFamily::PlayStation},
      {QStringLiteral("Sony DualShock 4"), ControllerFamily::PlayStation},
      {QStringLiteral("Steam Deck Controller"), ControllerFamily::SteamDeck},
      {QStringLiteral("Valve Steam Controller"), ControllerFamily::SteamDeck},
      {QStringLiteral("Generic USB Joystick"), ControllerFamily::Generic},
  }};

  for (const auto &[hint, expected] : cases) {
    QCOMPARE(ControllerGlyphs::detectFamily(hint), expected);
  }
}

void ControllerGlyphTests::unrecognizedOrEmptyHintsAreGeneric() {
  QCOMPARE(ControllerGlyphs::detectFamily(QString()),
           ControllerFamily::Generic);
  QCOMPARE(ControllerGlyphs::detectFamily(QStringLiteral("")),
           ControllerFamily::Generic);
  QCOMPARE(ControllerGlyphs::detectFamily(QStringLiteral("Some Unknown Pad")),
           ControllerFamily::Generic);
}

void ControllerGlyphTests::steamDeckIsCheckedBeforePlayStationAndXbox() {
  // A hint that could plausibly (if checked in the wrong order) match more
  // than one branch must still deterministically resolve to Steam Deck,
  // since it is checked first.
  QCOMPARE(ControllerGlyphs::detectFamily(
               QStringLiteral("Steam Deck (Xbox-compatible mode)")),
           ControllerFamily::SteamDeck);
}

void ControllerGlyphTests::everyFamilyGivesADistinctGlyphForEveryButton() {
  constexpr std::array families{
      ControllerFamily::Generic,
      ControllerFamily::Xbox,
      ControllerFamily::PlayStation,
      ControllerFamily::SteamDeck,
  };
  constexpr std::array buttons{
      GamepadButton::South,        GamepadButton::East,
      GamepadButton::West,         GamepadButton::North,
      GamepadButton::Start,        GamepadButton::Select,
      GamepadButton::ShoulderLeft, GamepadButton::ShoulderRight,
      GamepadButton::DpadUp,       GamepadButton::DpadDown,
      GamepadButton::DpadLeft,     GamepadButton::DpadRight,
  };

  for (const ControllerFamily family : families) {
    QSet<QString> seenLabels;
    for (const GamepadButton button : buttons) {
      const QString label = ControllerGlyphs::glyphLabel(family, button);
      QVERIFY(!label.isEmpty());
      // Within one family, no two distinct buttons may share a label
      // (that would make the glyph ambiguous to a player).
      QVERIFY(!seenLabels.contains(label));
      seenLabels.insert(label);
    }
    QCOMPARE(seenLabels.size(), static_cast<qsizetype>(buttons.size()));
  }
}

void ControllerGlyphTests::glyphLookupIsDeterministicAcrossRepeatedCalls() {
  const QString first = ControllerGlyphs::glyphLabel(ControllerFamily::Xbox,
                                                     GamepadButton::South);
  const QString second = ControllerGlyphs::glyphLabel(ControllerFamily::Xbox,
                                                      GamepadButton::South);
  QCOMPARE(first, second);
  QCOMPARE(first, QStringLiteral("A"));
}

QTEST_APPLESS_MAIN(ControllerGlyphTests)

#include "ControllerGlyphTests.moc"
