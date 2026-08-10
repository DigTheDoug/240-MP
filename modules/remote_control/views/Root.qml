import QtQuick
import Components

FocusScope {
    id: moduleRoot

    signal goBack()

    property var navParams: ({})

    property string moduleId: "com.240mp.remote_control"
    property var _moduleInfo: appCore ? appCore.get_module_info(moduleId) : ({})
    property string moduleName: _moduleInfo.name || ""
    property string moduleIcon: _moduleInfo.icon || ""

    property var commandLog: []

    Connections {
        target: remoteControlBackend
        function onCommandReceived(timestamp, sourceIp, service, ratingKey, accepted) {
            var list = moduleRoot.commandLog.slice()
            list.unshift({
                timestamp: timestamp, sourceIp: sourceIp, service: service,
                ratingKey: ratingKey, accepted: accepted
            })
            if (list.length > 50) list.length = 50
            moduleRoot.commandLog = list
        }
    }

    Component.onCompleted: {
        commandLog = remoteControlBackend.getCommandLog()
    }

    focus: true
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
            goBack()
            event.accepted = true
        }
    }

    AppBar {
        iconSource: moduleRoot.moduleIcon
        title: moduleRoot.moduleName
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.125
        anchors.leftMargin: root.sw * 0.125
    }

    // Status block: listening state, port, and the token to paste into Home
    // Assistant's rest_command. Enabling/disabling the listener happens on the
    // module's Settings screen (the manifest's "enabled" toggle) — this view
    // is read-only status + history.
    Column {
        id: statusColumn
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.25
        anchors.leftMargin: root.sw * 0.125
        spacing: root.sh * 0.015

        Text {
            text: remoteControlBackend.listening
                  ? "STATUS: LISTENING ON PORT " + remoteControlBackend.port
                  : (remoteControlBackend.lastError !== ""
                     ? "STATUS: ERROR — " + remoteControlBackend.lastError
                     : "STATUS: DISABLED")
            color: remoteControlBackend.listening ? root.primaryColor
                   : (remoteControlBackend.lastError !== "" ? root.accentColor : root.tertiaryColor)
            font.family: root.globalFont
            font.capitalization: Font.AllUppercase
            font.pixelSize: root.sh * 0.033
        }

        Text {
            visible: remoteControlBackend.listening
            text: "TOKEN: " + remoteControlBackend.token
            color: root.secondaryColor
            font.family: root.globalFont
            font.pixelSize: root.sh * 0.025
        }
    }

    // Live command log — most recent first, capped at 50 entries by the backend.
    Column {
        id: logHeader
        anchors.top: statusColumn.bottom
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.05
        anchors.leftMargin: root.sw * 0.125

        Text {
            text: "COMMAND LOG"
            color: root.tertiaryColor
            font.family: root.globalFont
            font.capitalization: Font.AllUppercase
            font.pixelSize: root.sh * 0.025
        }
    }

    ListView {
        id: logList
        anchors.top: logHeader.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.topMargin: root.sh * 0.02
        anchors.leftMargin: root.sw * 0.125
        anchors.rightMargin: root.sw * 0.125
        anchors.bottomMargin: root.sh * 0.15
        clip: true
        spacing: root.sh * 0.008
        model: moduleRoot.commandLog

        delegate: Text {
            width: logList.width
            text: modelData.timestamp + "  " + modelData.sourceIp + "  " +
                  modelData.service + "  " + modelData.ratingKey + "  " +
                  (modelData.accepted ? "ACCEPTED" : "REJECTED")
            color: modelData.accepted ? root.secondaryColor : root.tertiaryColor
            font.family: root.globalFont
            font.pixelSize: root.sh * 0.02
            elide: Text.ElideRight
        }
    }

    Text {
        text: root.hints.back + ":BACK"
        color: root.tertiaryColor
        font.family: root.globalFont
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.bottomMargin: root.sh * 0.1041667
        anchors.leftMargin: root.sw * 0.125
        font.pixelSize: root.sh * 0.0333333
    }
}
