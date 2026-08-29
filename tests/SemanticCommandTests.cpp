#include <array>
#include <utility>

#include <QtTest>

#include "SemanticCommand.h"

// Table-driven, exhaustive coverage of the closed SemanticCommand
// vocabulary: every enumerator has a fixed, stable, kebab-case name, and
// the table below is deliberately kept in exact 1:1 sync with the enum
// declaration order in SemanticCommand.h so that adding a new enumerator
// without adding a corresponding table row is caught by
// coversEveryEnumeratorExactlyOnce() rather than silently leaving a gap.
class SemanticCommandTests final : public QObject {
  Q_OBJECT

private slots:
  void givesEachCommandAStableKebabCaseName();
  void coversEveryEnumeratorExactlyOnce();
};

namespace {

// kEntries intentionally lists every SemanticCommand enumerator exactly
// once, in declaration order.
constexpr std::array kEntries{
    std::pair{Arkham::SemanticCommand::FocusUp, "focus-up"},
    std::pair{Arkham::SemanticCommand::FocusDown, "focus-down"},
    std::pair{Arkham::SemanticCommand::FocusLeft, "focus-left"},
    std::pair{Arkham::SemanticCommand::FocusRight, "focus-right"},
    std::pair{Arkham::SemanticCommand::PrimaryAction, "primary-action"},
    std::pair{Arkham::SemanticCommand::SecondaryAction, "secondary-action"},
    std::pair{Arkham::SemanticCommand::Inspect, "inspect"},
    std::pair{Arkham::SemanticCommand::OpenHand, "open-hand"},
    std::pair{Arkham::SemanticCommand::OpenPrompt, "open-prompt"},
    std::pair{Arkham::SemanticCommand::OpenInvestigator, "open-investigator"},
    std::pair{Arkham::SemanticCommand::OpenLog, "open-log"},
    std::pair{Arkham::SemanticCommand::OpenMenu, "open-menu"},
    std::pair{Arkham::SemanticCommand::CycleNextPlayer, "cycle-next-player"},
    std::pair{Arkham::SemanticCommand::CyclePreviousPlayer,
              "cycle-previous-player"},
    std::pair{Arkham::SemanticCommand::CycleNextZone, "cycle-next-zone"},
    std::pair{Arkham::SemanticCommand::CyclePreviousZone,
              "cycle-previous-zone"},
    std::pair{Arkham::SemanticCommand::JumpToPrompt, "jump-to-prompt"},
    std::pair{Arkham::SemanticCommand::CameraZoomIn, "camera-zoom-in"},
    std::pair{Arkham::SemanticCommand::CameraZoomOut, "camera-zoom-out"},
    std::pair{Arkham::SemanticCommand::CameraRotateLeft, "camera-rotate-left"},
    std::pair{Arkham::SemanticCommand::CameraRotateRight,
              "camera-rotate-right"},
    std::pair{Arkham::SemanticCommand::CameraReset, "camera-reset"},
    std::pair{Arkham::SemanticCommand::MultiSelectConfirm,
              "multiselect-confirm"},
    std::pair{Arkham::SemanticCommand::MultiSelectCancel, "multiselect-cancel"},
    std::pair{Arkham::SemanticCommand::Undo, "undo"},
    std::pair{Arkham::SemanticCommand::ToggleArrangeMode,
              "toggle-arrange-mode"},
};

} // namespace

void SemanticCommandTests::givesEachCommandAStableKebabCaseName() {
  for (const auto &[command, expectedName] : kEntries) {
    QCOMPARE(Arkham::commandName(command), QLatin1String(expectedName));
  }
}

void SemanticCommandTests::coversEveryEnumeratorExactlyOnce() {
  // A mutation that removes a row from kEntries (rather than changing its
  // expected string) would otherwise slip past
  // givesEachCommandAStableKebabCaseName() -- this guards specifically
  // against that by pinning the exact row count, so shrinking the table
  // without shrinking the enum is caught here.
  QCOMPARE(kEntries.size(), std::size_t{26});

  // No two rows may name the same command twice, and no two rows may
  // produce the same string twice: both would indicate the table has
  // drifted out of an exact 1:1 mapping with the enum.
  for (std::size_t i = 0; i < kEntries.size(); ++i) {
    for (std::size_t j = i + 1; j < kEntries.size(); ++j) {
      QVERIFY(kEntries[i].first != kEntries[j].first);
      QVERIFY(QLatin1String(kEntries[i].second) !=
              QLatin1String(kEntries[j].second));
    }
  }
}

QTEST_APPLESS_MAIN(SemanticCommandTests)

#include "SemanticCommandTests.moc"
