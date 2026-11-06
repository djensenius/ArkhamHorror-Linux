pragma ComponentBehavior: Bound

import QtQuick

// A grid of the cards on a board snapshot. `model` is a BoardCardsModel
// (src/BoardCardsModel.h): one row per card with `kind`, `label` and
// `imageSource` roles. While the grid has keyboard focus, the current card
// carries a visible border (currentCardIndicator).
GridView {
    id: root

    // A model reset leaves no current card; with focus there must be one
    // so the indicator shows where the arrow keys start.
    function ensureCurrent() {
        if (root.activeFocus && root.currentIndex < 0 && root.count > 0)
            root.currentIndex = 0;
    }

    cellWidth: 160
    cellHeight: 220
    clip: true
    onActiveFocusChanged: root.ensureCurrent()
    onCountChanged: root.ensureCurrent()

    delegate: Item {
        id: delegate

        required property int index
        required property string kind
        required property string imageSource
        required property string label
        readonly property alias status: card.status

        objectName: "boardCard"
        width: root.cellWidth
        height: root.cellHeight
        Accessible.role: Accessible.ListItem
        Accessible.name: delegate.label
        Accessible.selected: delegate.GridView.isCurrentItem

        CardImage {
            id: card

            anchors.fill: parent
            anchors.margins: 5
            source: delegate.imageSource
            // The provider decodes and caches the art at the cell's size
            // (AssetCache keys entries by size), so a full board holds only
            // cell-sized decodes rather than full-resolution art.
            sourceSize: Qt.size(root.cellWidth - 10, root.cellHeight - 10)
            label: delegate.label
        }

        // Where arrow keys are: shown only while the grid has focus, in the
        // same bone colour as Main.qml's focused borders.
        Rectangle {
            objectName: "currentCardIndicator"
            anchors.fill: parent
            anchors.margins: 1
            visible: delegate.GridView.isCurrentItem && root.activeFocus
            color: "transparent"
            radius: 10
            border {
                color: "#ede1c3"
                width: 3
            }
        }
    }
}
