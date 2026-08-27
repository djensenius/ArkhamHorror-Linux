pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root

    width: 1280
    height: 800
    minimumWidth: 960
    minimumHeight: 600
    visible: true
    title: qsTr("Arkham Horror")
    color: "#081515"

    property color ink: "#081515"
    property color teal: "#123c3b"
    property color bone: "#ede1c3"
    property color gold: "#b7944c"
    required property string configuredServer

    background: Rectangle {
        gradient: Gradient {
            GradientStop {
                position: 0
                color: root.teal
            }
            GradientStop {
                position: 1
                color: root.ink
            }
        }
    }

    ColumnLayout {
        anchors {
            fill: parent
            margins: 48
        }
        spacing: 28

        Label {
            text: qsTr("ARKHAM HORROR")
            color: root.bone
            font {
                pixelSize: 42
                weight: Font.DemiBold
                letterSpacing: 4
            }
            Accessible.name: text
        }

        Label {
            Layout.maximumWidth: 780
            text: qsTr("A controller-first native client foundation. The game board will expose relevant actions directly instead of simulating a mouse or physical tabletop.")
            color: "#d8cfb7"
            font.pixelSize: 22
            wrapMode: Text.WordWrap
        }

        RowLayout {
            spacing: 20

            Repeater {
                id: destinations

                model: [
                    {
                        title: qsTr("Connect"),
                        detail: root.configuredServer
                    },
                    {
                        title: qsTr("Browse fixtures"),
                        detail: qsTr("Contract gallery")
                    },
                    {
                        title: qsTr("Input guide"),
                        detail: qsTr("Controller and keyboard")
                    }
                ]

                delegate: Button {
                    id: destination

                    required property int index
                    required property var modelData
                    Layout.preferredWidth: 280
                    Layout.preferredHeight: 150
                    activeFocusOnTab: true
                    focus: index === 0
                    text: modelData.title

                    KeyNavigation.left: index > 0 ? destinations.itemAt(index - 1) : null
                    KeyNavigation.right: index < 2 ? destinations.itemAt(index + 1) : null

                    contentItem: Column {
                        anchors {
                            fill: parent
                            margins: 20
                        }
                        spacing: 8

                        Label {
                            text: destination.modelData.title
                            color: destination.activeFocus ? root.ink : root.bone
                            font {
                                pixelSize: 24
                                weight: Font.DemiBold
                            }
                        }

                        Label {
                            width: parent.width
                            text: destination.modelData.detail
                            color: destination.activeFocus ? "#153332" : "#c1b99f"
                            elide: Text.ElideRight
                            font.pixelSize: 16
                        }
                    }

                    background: Rectangle {
                        radius: 18
                        color: destination.activeFocus ? root.gold : "#182c2c"
                        border {
                            color: destination.activeFocus ? root.bone : "#35504f"
                            width: destination.activeFocus ? 3 : 1
                        }
                        scale: destination.activeFocus ? 1.035 : 1
                        Behavior on scale {
                            NumberAnimation {
                                duration: 120
                            }
                        }
                    }

                    Accessible.name: destination.modelData.title
                    Accessible.description: destination.modelData.detail
                }
            }
        }

        Item {
            Layout.fillHeight: true
        }

        Label {
            text: qsTr("Directional input moves focus | Enter / A selects | Escape / B returns")
            color: "#a9a189"
            font.pixelSize: 17
        }
    }
}
