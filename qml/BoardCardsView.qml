pragma ComponentBehavior: Bound

import QtQuick

// A grid of the cards on a board snapshot. `model` is a BoardCardsModel
// (src/BoardCardsModel.h): one row per card with `kind`, `label` and
// `imageSource` roles.
GridView {
    id: root

    cellWidth: 160
    cellHeight: 220
    clip: true

    delegate: Item {
        id: delegate

        required property string kind
        required property string imageSource
        required property string label
        readonly property alias status: card.status

        objectName: "boardCard"
        width: root.cellWidth
        height: root.cellHeight

        CardImage {
            id: card

            anchors.fill: parent
            anchors.margins: 5
            source: delegate.imageSource
            label: delegate.label
        }
    }
}
