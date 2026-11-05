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

    // Prefers the coordinator's actual selected profile once one has been
    // loaded (see SessionCoordinator::selectedProfileBaseUrl); only falls
    // back to the static |configuredServer| initial property (the hosted
    // default) before the coordinator has started, or in --smoke-test
    // where sessionCoordinator is never registered at all. This avoids the
    // "Connect" card ever showing a server other than the one actually in
    // use. Never displays a password, token, or Authorization header --
    // only a public server URL.
    // qmllint disable unqualified
    function connectDetail() {
        if (typeof sessionCoordinator !== "undefined" && sessionCoordinator && sessionCoordinator.selectedProfileBaseUrl.length > 0)
            return sessionCoordinator.selectedProfileBaseUrl;
        return root.configuredServer;
    }
    // qmllint enable unqualified

    // Shows the games/board screen in place of the landing page (a Loader,
    // so the screen and its requests exist only while it is open).
    function openGames() {
        boardLoader.active = true;
    }
    function closeGames() {
        boardLoader.active = false;
        const gamesCard = destinations.itemAt(3);
        if (gamesCard)
            gamesCard.forceActiveFocus();
    }

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
        objectName: "landing"
        visible: !boardLoader.active
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
                        detail: root.connectDetail()
                    },
                    {
                        title: qsTr("Browse fixtures"),
                        detail: qsTr("Contract gallery")
                    },
                    {
                        title: qsTr("Input guide"),
                        detail: qsTr("Controller and keyboard")
                    },
                    {
                        title: qsTr("Your games"),
                        detail: qsTr("Open a game's board"),
                        opensGames: true
                    }
                ]

                onItemAdded: (index, item) => {
                    if (index === 0)
                        item.forceActiveFocus();
                }

                delegate: Button {
                    id: destination

                    required property int index
                    required property var modelData
                    Layout.preferredWidth: 280
                    Layout.preferredHeight: 150
                    activeFocusOnTab: true
                    text: modelData.title
                    onClicked: {
                        if (destination.modelData.opensGames === true)
                            root.openGames();
                    }
                    Keys.onReturnPressed: destination.clicked()
                    Keys.onEnterPressed: destination.clicked()

                    KeyNavigation.left: index > 0 ? destinations.itemAt(index - 1) : null
                    KeyNavigation.right: index + 1 < destinations.count ? destinations.itemAt(index + 1) : null
                    // A shown maintenance notice joins the directional chain
                    // so a controller can scroll it (see maintenanceNotice).
                    KeyNavigation.down: maintenanceNotice.visible ? maintenanceNotice : null

                    onActiveFocusChanged: {
                        if (activeFocus)
                            maintenanceNotice.returnFocusItem = destination;
                    }

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
            // sessionCoordinator is only registered as a context property
            // during normal startup (see AppBootstrap.h / main.cpp); it is
            // deliberately absent in --smoke-test, so this guards against
            // referencing an undeclared identifier rather than assuming it
            // always exists. Only ever shows non-secret state/server
            // metadata: never a password, token, or Authorization header.
            // qmllint disable unqualified
            text: typeof sessionCoordinator !== "undefined" && sessionCoordinator ? qsTr("%1 — %2").arg(sessionCoordinator.stateDescription).arg(sessionCoordinator.selectedProfileDisplayName) : ""
            // qmllint enable unqualified
            objectName: "statusLabel"
            visible: text.length > 0
            // A custom profile name can be arbitrarily long: keep it to one
            // line inside the window and expose the full text to assistive
            // technology (a controller cannot reach a tooltip).
            Layout.fillWidth: true
            elide: Text.ElideRight
            maximumLineCount: 1
            color: root.gold
            font.pixelSize: 16
            Accessible.name: text
        }

        ScrollView {
            id: maintenanceNotice

            // The server's maintenance notice (its own user-facing message
            // plus when to try again); shown only in ServerMaintenance. A
            // valid message can hold many lines, so the notice is height-
            // bounded and scrolls instead of pushing the layout off-screen;
            // the whole text, including "Try again ...", stays reachable.
            // It is also reachable without a pointer: Down from a
            // destination card focuses it, Down/Up then scroll it, and Up at
            // the top returns focus to the card it was entered from.
            objectName: "maintenanceNotice"
            // The destination card that last had focus (Up at the top goes
            // back to it).
            property Item returnFocusItem: null
            // Keeps a few lines of overlap between successive steps.
            readonly property real scrollStep: Math.max(maintenanceText.font.pixelSize * 2, availableHeight * 0.75)

            function scrollNotice(delta: real): bool {
                const flick = contentItem as Flickable;
                if (!flick)
                    return false;
                const maximumY = Math.max(0, flick.contentHeight - flick.height);
                const nextY = Math.min(maximumY, Math.max(0, flick.contentY + delta));
                if (nextY === flick.contentY)
                    return false;
                flick.contentY = nextY;
                return true;
            }

            activeFocusOnTab: true
            focusPolicy: Qt.StrongFocus
            // When maintenance ends while the notice holds focus, hand focus
            // back to a card instead of stranding it on a hidden item.
            onVisibleChanged: {
                if (!visible && activeFocus) {
                    const target = maintenanceNotice.returnFocusItem ?? destinations.itemAt(0);
                    if (target)
                        target.forceActiveFocus();
                }
            }
            Keys.onDownPressed: event => {
                maintenanceNotice.scrollNotice(maintenanceNotice.scrollStep);
                event.accepted = true;
            }
            Keys.onUpPressed: event => {
                if (!maintenanceNotice.scrollNotice(-maintenanceNotice.scrollStep)) {
                    const target = maintenanceNotice.returnFocusItem ?? destinations.itemAt(0);
                    if (target)
                        target.forceActiveFocus();
                }
                event.accepted = true;
            }
            Accessible.role: Accessible.StaticText
            Accessible.name: maintenanceText.text
            Accessible.focusable: true

            // Visible focus indicator, matching the destination cards'
            // focused border colour.
            background: Rectangle {
                color: "transparent"
                radius: 6
                border {
                    color: root.bone
                    width: maintenanceNotice.activeFocus ? 2 : 0
                }
            }
            visible: maintenanceText.text.length > 0
            clip: true
            Layout.fillWidth: true
            Layout.maximumWidth: 720
            // Grows into the free space (shared with the spacer above) up to
            // a fifth of the window, and never below about two lines.
            Layout.fillHeight: true
            Layout.minimumHeight: Math.min(maintenanceText.implicitHeight, maintenanceText.font.pixelSize * 2.5)
            Layout.preferredHeight: Layout.minimumHeight
            Layout.maximumHeight: Math.min(maintenanceText.implicitHeight, root.height * 0.2)
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            Label {
                id: maintenanceText

                objectName: "maintenanceNoticeText"
                width: maintenanceNotice.availableWidth
                // qmllint disable unqualified
                text: typeof sessionCoordinator !== "undefined" && sessionCoordinator && sessionCoordinator.serverMaintenance ? sessionCoordinator.diagnostic : ""
                // qmllint enable unqualified
                // Server-written free text: never render it as markup (no
                // remote images or links from the message).
                textFormat: Text.PlainText
                // The notice itself announces the whole text; don't read it
                // twice.
                Accessible.ignored: true
                // Wrap (not WordWrap): an over-wide word such as a long URL
                // also breaks inside the viewport, which never scrolls
                // horizontally.
                wrapMode: Text.Wrap
                color: root.bone
                font.pixelSize: 16
            }
        }

        Label {
            objectName: "inputHint"
            text: qsTr("Directional input moves focus | Enter / A selects | Escape / B returns")
            color: "#a9a189"
            font.pixelSize: 17
        }
    }

    Loader {
        id: boardLoader

        objectName: "boardLoader"
        anchors.fill: parent
        active: false
        focus: active
        sourceComponent: BoardScreen {
            // gameBoard is only registered during normal startup (see
            // main.cpp); it is absent in --smoke-test.
            // qmllint disable unqualified
            controller: typeof gameBoard !== "undefined" ? gameBoard : null
            // qmllint enable unqualified
            onClosed: root.closeGames()
        }
        onLoaded: boardLoader.forceActiveFocus()
    }
}
