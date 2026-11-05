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
    // Signed out (or the session reset the list): nothing to show until a
    // sign-in, or a manual Reload.
    readonly property bool needsSignIn: !root.controller || (!root.showingBoard && root.listStatus === "idle")
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
        else if (root.needsSignIn && root.controller)
            reloadButton.forceActiveFocus();
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

    // Game names are server text: show them on one line (a name may hold
    // newlines); accessible names keep the full text.
    function singleLine(text: string): string {
        return text.replace(/[\r\n\v\f\u0085\u2028\u2029]+/g, " ");
    }

    // Back to SignedIn while open: reload whatever the session cleared. A
    // list still "loading" was requested while signed out, so its token
    // reply can only be the signed-out one: reload it too (loadGames()
    // starts a new generation, so that stale reply is dropped).
    Connections {
        target: root.controller
        ignoreUnknownSignals: true

        function onSignedIn() {
            if (root.listStatus !== "loaded")
                root.controller.loadGames();
        }
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
            id: boardTitle

            readonly property string fullTitle: root.showingBoard && root.controller ? root.controller.selectedTitle : qsTr("Your games")

            objectName: "boardTitle"
            Layout.fillWidth: true
            text: root.singleLine(boardTitle.fullTitle)
            textFormat: Text.PlainText
            elide: Text.ElideRight
            maximumLineCount: 1
            color: root.bone
            font {
                pixelSize: 32
                weight: Font.DemiBold
            }
            Accessible.name: boardTitle.fullTitle
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
            visible: root.needsSignIn
            text: qsTr("Sign in to see your games.")
            color: root.bone
            font.pixelSize: 20
            Accessible.name: text
        }

        // The error, which may be a server-written maintenance notice of many
        // lines: height-bounded and scrolling (as Main.qml's
        // maintenanceNotice) so Retry and Back stay on screen. Up from the
        // buttons focuses it; Up/Down scroll it, and Down at the end returns
        // to the buttons.
        ScrollView {
            id: errorNotice

            readonly property real scrollStep: Math.max(errorText.font.pixelSize * 2, availableHeight * 0.75)

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

            objectName: "errorNotice"
            visible: root.stageStatus === "error"
            // A Tab stop only when there is something to scroll; Up from
            // Retry still reaches it either way.
            focusPolicy: errorNotice.contentHeight > errorNotice.availableHeight ? Qt.StrongFocus : Qt.ClickFocus
            Keys.onDownPressed: event => {
                if (!errorNotice.scrollNotice(errorNotice.scrollStep))
                    retryButton.forceActiveFocus();
                event.accepted = true;
            }
            Keys.onUpPressed: event => {
                errorNotice.scrollNotice(-errorNotice.scrollStep);
                event.accepted = true;
            }
            Accessible.role: Accessible.AlertMessage
            Accessible.name: errorText.text
            Accessible.focusable: true
            background: Rectangle {
                color: "transparent"
                radius: 6
                border {
                    color: root.bone
                    width: errorNotice.activeFocus ? 2 : 0
                }
            }
            clip: true
            Layout.fillWidth: true
            Layout.maximumWidth: 780
            // At most a third of the screen; never below about two lines.
            Layout.fillHeight: true
            Layout.minimumHeight: Math.min(errorText.implicitHeight, errorText.font.pixelSize * 2.5)
            Layout.preferredHeight: Layout.minimumHeight
            Layout.maximumHeight: Math.min(errorText.implicitHeight, root.height * 0.3)
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            Label {
                id: errorText

                objectName: "errorLabel"
                width: errorNotice.availableWidth
                text: root.controller ? root.controller.errorText : ""
                // Server-written text (maintenance notices): never markup.
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: root.gold
                font.pixelSize: 20
                // The notice announces the whole text; don't read it twice.
                Accessible.ignored: true
            }
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
            visible: !root.showingBoard && root.listStatus === "loaded" && gamesList.count > 0
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
                    objectName: "gameRowLabel"
                    text: root.singleLine(gameRow.text)
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    maximumLineCount: 1
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
                Accessible.selected: gameRow.ListView.isCurrentItem
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
                id: reloadButton

                objectName: "reloadButton"
                visible: root.needsSignIn && !!root.controller
                text: qsTr("Reload")
                activeFocusOnTab: true
                KeyNavigation.right: backButton
                onClicked: root.controller.loadGames()
                Keys.onReturnPressed: reloadButton.clicked()
                Keys.onEnterPressed: reloadButton.clicked()
                Accessible.name: text
            }

            Button {
                id: retryButton

                objectName: "retryButton"
                visible: root.stageStatus === "error"
                text: qsTr("Retry")
                activeFocusOnTab: true
                KeyNavigation.right: backButton
                KeyNavigation.up: errorNotice
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
                KeyNavigation.left: retryButton.visible ? retryButton : (reloadButton.visible ? reloadButton : null)
                KeyNavigation.up: errorNotice.visible ? errorNotice : (gamesList.visible ? gamesList : (board.visible ? board : null))
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
