import Toybox.Graphics;
import Toybox.Lang;
import Toybox.WatchUi;

// Home screen: bridge link state, current treadmill, and the picker hint.
class StatusView extends WatchUi.View {
    hidden var mBle as BridgeBle;

    function initialize(ble as BridgeBle) {
        View.initialize();
        mBle = ble;
    }

    function onUpdate(dc as Dc) as Void {
        dc.setColor(Graphics.COLOR_WHITE, Graphics.COLOR_BLACK);
        dc.clear();

        var w = dc.getWidth();
        var h = dc.getHeight();

        var bridge = "searching…";
        if (mBle.isConnected()) {
            bridge = "connected";
        } else if (mBle.isScanning()) {
            bridge = "scanning…";
        }
        dc.drawText(w / 2, h / 5, Graphics.FONT_XTINY, "bridge " + bridge,
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);

        var line1 = "no treadmill";
        var line2 = "";
        if (mBle.isConnected()) {
            if (mBle.tmConnected) {
                line1 = mBle.tmName.length() > 0 ? mBle.tmName : "(unnamed)";
                line2 = mBle.tmProto;
            } else {
                line1 = "auto-connecting";
                line2 = "last / closest";
            }
        }
        dc.drawText(w / 2, h * 2 / 5, Graphics.FONT_MEDIUM, line1,
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);
        dc.drawText(w / 2, h * 11 / 20, Graphics.FONT_XTINY, line2,
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);

        if (mBle.isConnected()) {
            dc.drawText(w / 2, h * 3 / 4, Graphics.FONT_XTINY,
                "START: pick treadmill",
                Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);
        }
    }
}
