pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A small, 1280x800-first review/test fixture for the semantic focus and
// input foundation (see FocusController/InputMapper/InputRouter in
// src/). It is deliberately NOT wired into main.cpp/Main.qml: it exists
// purely so this slice's focus-graph and input-routing behavior can be
// exercised end-to-end through real QML, with real accessible names/
// roles and real native Qt focus (see the itemsById/forceActiveFocus
// wiring below), without adding any live backend/network/board/asset
// implementation. All command-to-focus-graph routing (which physical
// command calls which FocusController method) happens in C++ (see
// tests/SemanticInputFixtureTests.cpp), never here: this file only ever
// reflects focusController.currentFocusId back into real QML focus and
// renders static, fixed fixture content. No virtual cursor, no pointer
// simulation: focus rectangles are drawn purely from
// Item.activeFocus, which itself only ever changes via
// forceActiveFocus() below.
ApplicationWindow {
    id: root

    width: 1280
    height: 800
    visible: true
    title: qsTr("Semantic Input Fixture")
    color: "#081515"

    // Set by the C++ test harness via QQmlApplicationEngine's initial
    // properties, exactly like Main.qml's `configuredServer` -- see
    // SemanticInputFixtureTests.cpp. Deliberately untyped (`var`, not
    // `QtObject`), matching Main.qml's sessionCoordinator context
    // property: FocusController is not registered as a QML type (see
    // FocusController.h's comment on why), so a declared QtObject type
    // here would make qmllint flag every one of its real properties as
    // unknown members on a plain QtObject.
    required property var focusController

    // Maps each fixture node id (see the "board"/"hand"/"log" delegates
    // below) to its actual QML Item, so real forceActiveFocus() can be
    // called on exactly the item FocusController now considers focused.
    property var itemsById: ({})

    function registerItem(nodeId, item) {
        itemsById[nodeId] = item;
    }

    function syncActiveFocus() {
        const item = itemsById[focusController.currentFocusId];
        if (item)
            item.forceActiveFocus();
    }

    Connections {
        target: root.focusController
        function onCurrentFocusChanged() {
            root.syncActiveFocus();
        }
    }

    Component.onCompleted: syncActiveFocus()

    RowLayout {
        anchors {
            fill: parent
            margins: 32
        }
        spacing: 24

        // The "board" zone: a 2x2 grid with full four-directional
        // adjacency, registered by the C++ test harness as zone "board".
        GridLayout {
            id: boardZone

            objectName: "boardZone"
            columns: 2
            rowSpacing: 16
            columnSpacing: 16
            Layout.preferredWidth: 420

            Repeater {
                model: [
                    {
                        id: "board.nw",
                        label: qsTr("Northwest Zone")
                    },
                    {
                        id: "board.ne",
                        label: qsTr("Northeast Zone")
                    },
                    {
                        id: "board.sw",
                        label: qsTr("Southwest Zone")
                    },
                    {
                        id: "board.se",
                        label: qsTr("Southeast Zone")
                    }
                ]

                delegate: Rectangle {
                    id: boardDelegate

                    required property var modelData
                    Layout.preferredWidth: 190
                    Layout.preferredHeight: 190
                    radius: 12
                    color: activeFocus ? "#b7944c" : "#182c2c"
                    border {
                        color: activeFocus ? "#ede1c3" : "#35504f"
                        width: activeFocus ? 3 : 1
                    }
                    activeFocusOnTab: true

                    Accessible.role: Accessible.Button
                    Accessible.name: boardDelegate.modelData.label
                    Accessible.focusable: true
                    Accessible.focused: boardDelegate.activeFocus

                    Component.onCompleted: root.registerItem(boardDelegate.modelData.id, boardDelegate)

                    Label {
                        anchors.centerIn: parent
                        text: boardDelegate.modelData.label
                        color: boardDelegate.activeFocus ? "#153332" : "#ede1c3"
                        wrapMode: Text.WordWrap
                        width: parent.width - 16
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }
        }

        // The "hand" zone: a row of cards with only left/right adjacency,
        // registered by the C++ test harness as zone "hand".
        ColumnLayout {
            objectName: "handZone"
            Layout.preferredWidth: 360
            spacing: 12

            Label {
                text: qsTr("Hand")
                color: "#b7944c"
                font.pixelSize: 20
            }

            RowLayout {
                spacing: 12

                Repeater {
                    model: [
                        {
                            id: "hand.card1",
                            label: qsTr("Card 1")
                        },
                        {
                            id: "hand.card2",
                            label: qsTr("Card 2")
                        },
                        {
                            id: "hand.card3",
                            label: qsTr("Card 3")
                        }
                    ]

                    delegate: Rectangle {
                        id: handDelegate

                        required property var modelData
                        Layout.preferredWidth: 100
                        Layout.preferredHeight: 150
                        radius: 10
                        color: activeFocus ? "#b7944c" : "#182c2c"
                        border {
                            color: activeFocus ? "#ede1c3" : "#35504f"
                            width: activeFocus ? 3 : 1
                        }
                        activeFocusOnTab: true

                        Accessible.role: Accessible.Button
                        Accessible.name: handDelegate.modelData.label
                        Accessible.focusable: true
                        Accessible.focused: handDelegate.activeFocus

                        Component.onCompleted: root.registerItem(handDelegate.modelData.id, handDelegate)

                        Label {
                            anchors.centerIn: parent
                            text: handDelegate.modelData.label
                            color: handDelegate.activeFocus ? "#153332" : "#ede1c3"
                        }
                    }
                }
            }
        }

        // The "log" zone: a single entry, registered by the C++ test
        // harness as zone "log".
        ColumnLayout {
            objectName: "logZone"
            Layout.preferredWidth: 300
            spacing: 12

            Label {
                text: qsTr("Log")
                color: "#b7944c"
                font.pixelSize: 20
            }

            Rectangle {
                id: logDelegate

                Layout.preferredWidth: 260
                Layout.preferredHeight: 80
                radius: 10
                color: activeFocus ? "#b7944c" : "#182c2c"
                border {
                    color: activeFocus ? "#ede1c3" : "#35504f"
                    width: activeFocus ? 3 : 1
                }
                activeFocusOnTab: true

                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Log Entry")
                Accessible.focusable: true
                Accessible.focused: logDelegate.activeFocus

                Component.onCompleted: root.registerItem("log.entry", logDelegate)

                Label {
                    anchors.centerIn: parent
                    text: qsTr("Log Entry")
                    color: logDelegate.activeFocus ? "#153332" : "#ede1c3"
                }
            }
        }
    }

    Label {
        anchors {
            bottom: parent.bottom
            horizontalCenter: parent.horizontalCenter
            margins: 16
        }
        text: qsTr("Focused: %1").arg(root.focusController.currentFocusId)
        color: "#a9a189"
        font.pixelSize: 16
    }
}
