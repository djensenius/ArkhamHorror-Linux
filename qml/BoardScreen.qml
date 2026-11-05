pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The signed-in user's games, then one game's board. `controller` is a
// GameBoardController (src/GameBoardController.h), or null when there is no
// session (e.g. --smoke-test). Escape / B goes from the board back to the
// list, and from the list back out (closed()).
FocusScope {
    id: root

    property var controller: null

    readonly property string listStatus: root.controller ? root.controller.listStatus : "idle"
    readonly property string boardStatus: root.controller ? root.controller.boardStatus : "idle"
    readonly property bool showingBoard: root.boardStatus !== "idle"
    readonly property string stageStatus: root.showingBoard ? root.boardStatus : root.listStatus
    readonly property color bone: "#ede1c3"
    readonly property color gold: "#b7944c"

    signal closed

    function back() {
        if (root.showingBoard && root.controller)
            root.controller.closeGame();
        else
            root.closed();
    }

    // Puts focus on whatever the current stage shows.
    function focusStage() {
        if (root.stageStatus === "error")
            retryButton.forceActiveFocus();
        else if (root.showingBoard && root.boardStatus === "loaded")
            board.forceActiveFocus();
        else if (!root.showingBoard && root.listStatus === "loaded" && gamesList.count > 0)
            gamesList.forceActiveFocus();
        else
            backButton.forceActiveFocus();
    }

    // Deferred so the stages' visible bindings and the list's count have
    // caught up with the status change first.
    onStageStatusChanged: Qt.callLater(root.focusStage)
    onShowingBoardChanged: Qt.callLater(root.focusStage)
    Component.onCompleted: {
        if (root.controller)
            root.controller.loadGames();
        root.focusStage();
    }

    Keys.onEscapePressed: event => {
        root.back();
        event.accepted = true;
    }
    Keys.onBackPressed: event => {
        root.back();
        event.accepted = true;
    }

    ColumnLayout {
        anchors {
            fill: parent
            margins: 48
        }
        spacing: 20

        Label {
            objectName: "boardTitle"
            Layout.fillWidth: true
            text: root.showingBoard && root.controller ? root.controller.selectedTitle : qsTr("Your games")
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: root.bone
            font {
                pixelSize: 32
                weight: Font.DemiBold
            }
            Accessible.name: text
        }

        Label {
            objectName: "loadingLabel"
            visible: root.stageStatus === "loading"
            text: root.showingBoard ? qsTr("Loading the board…") : qsTr("Loading your games…")
            color: root.bone
            font.pixelSize: 20
            Accessible.name: text
        }

        Label {
            objectName: "signedOutLabel"
            visible: !root.controller
            text: qsTr("Sign in to see your games.")
            color: root.bone
            font.pixelSize: 20
            Accessible.name: text
        }

        Label {
            objectName: "errorLabel"
            visible: root.stageStatus === "error"
            Layout.fillWidth: true
            Layout.maximumWidth: 780
            text: root.controller ? root.controller.errorText : ""
            // Server-written text (maintenance notices): never markup.
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: root.gold
            font.pixelSize: 20
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }

        Label {
            objectName: "emptyLabel"
            visible: !root.showingBoard && root.listStatus === "loaded" && gamesList.count === 0
            text: qsTr("You have no games yet.")
            color: root.bone
            font.pixelSize: 20
            Accessible.name: text
        }

        ListView {
            id: gamesList

            objectName: "gamesList"
            visible: !root.showingBoard && root.listStatus === "loaded"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.maximumWidth: 780
            clip: true
            spacing: 8
            keyNavigationEnabled: true
            model: root.controller ? root.controller.gameTitles : []
            KeyNavigation.down: backButton

            Keys.onReturnPressed: event => {
                root.controller.openGame(gamesList.currentIndex);
                event.accepted = true;
            }
            Keys.onEnterPressed: event => {
                root.controller.openGame(gamesList.currentIndex);
                event.accepted = true;
            }

            delegate: ItemDelegate {
                id: gameRow

                required property int index
                required property string modelData

                width: ListView.view.width
                text: gameRow.modelData
                highlighted: ListView.isCurrentItem && gamesList.activeFocus
                focusPolicy: Qt.NoFocus
                onClicked: root.controller.openGame(gameRow.index)

                contentItem: Label {
                    text: gameRow.text
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: gameRow.highlighted ? "#081515" : root.bone
                    font.pixelSize: 20
                }
                background: Rectangle {
                    radius: 10
                    color: gameRow.highlighted ? root.gold : "#182c2c"
                    border.color: gameRow.highlighted ? root.bone : "#35504f"
                }

                Accessible.role: Accessible.ListItem
                Accessible.name: gameRow.modelData
            }

            Accessible.role: Accessible.List
            Accessible.name: qsTr("Your games")
        }

        BoardCardsView {
            id: board

            objectName: "board"
            visible: root.showingBoard && root.boardStatus === "loaded"
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: root.controller ? root.controller.boardCards : null
            keyNavigationEnabled: true
            KeyNavigation.down: backButton
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Cards on the board")
        }

        Item {
            Layout.fillHeight: true
            visible: !gamesList.visible && !board.visible
        }

        RowLayout {
            spacing: 16

            Button {
                id: retryButton

                objectName: "retryButton"
                visible: root.stageStatus === "error"
                text: qsTr("Retry")
                activeFocusOnTab: true
                KeyNavigation.right: backButton
                onClicked: root.controller.retry()
                Keys.onReturnPressed: retryButton.clicked()
                Keys.onEnterPressed: retryButton.clicked()
                Accessible.name: text
            }

            Button {
                id: backButton

                objectName: "backButton"
                text: root.showingBoard ? qsTr("Back to games") : qsTr("Back")
                activeFocusOnTab: true
                KeyNavigation.left: retryButton.visible ? retryButton : null
                KeyNavigation.up: gamesList.visible ? gamesList : (board.visible ? board : null)
                onClicked: root.back()
                Keys.onReturnPressed: backButton.clicked()
                Keys.onEnterPressed: backButton.clicked()
                Accessible.name: text
            }
        }

        Label {
            text: qsTr("Directional input moves focus | Enter / A selects | Escape / B returns")
            color: "#a9a189"
            font.pixelSize: 17
        }
    }
}
