import QtQuick

// One card's art, loaded through the image://arkham-card provider (see
// src/AssetImageProvider.h). The image loads asynchronously; a placeholder
// with the card's label covers it until it is Ready, and an error state
// replaces it if the art cannot be fetched or decoded.
Item {
    id: root

    // An image://arkham-card/... source, e.g. from BoardCardsModel.
    property url source
    // Shown by the placeholder and the error state, and used as the
    // accessible name.
    property string label
    readonly property int status: image.status
    // Size requested from the image provider (default: full size). The
    // arkham-card provider decodes and caches the art fitted to this size
    // (never upscaled), so smaller sizes hold less decoded memory.
    property alias sourceSize: image.sourceSize

    implicitWidth: 150
    implicitHeight: 210
    Accessible.role: Accessible.Graphic
    Accessible.name: root.label

    Image {
        id: image

        anchors.fill: parent
        asynchronous: true
        cache: true
        fillMode: Image.PreserveAspectFit
        source: root.source
    }

    Rectangle {
        objectName: "cardPlaceholder"
        anchors.fill: parent
        visible: image.status !== Image.Ready
        radius: 8
        color: image.status === Image.Error ? "#3a1414" : "#1c2b2b"
        border.color: "#4a6363"

        Text {
            objectName: "cardLabel"
            anchors.fill: parent
            anchors.margins: 8
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
            // The label is server data (location labels, investigator
            // names); never interpret it as rich text or fetch its images.
            textFormat: Text.PlainText
            color: "#d8e4e4"
            text: image.status === Image.Error ? qsTr("%1\n(art unavailable)").arg(root.label) : root.label
        }
    }
}
