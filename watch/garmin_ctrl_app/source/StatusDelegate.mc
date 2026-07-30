import Toybox.Lang;
import Toybox.WatchUi;

class StatusDelegate extends WatchUi.BehaviorDelegate {
    hidden var mBle as BridgeBle;

    function initialize(ble as BridgeBle) {
        BehaviorDelegate.initialize();
        mBle = ble;
    }

    // START/enter (or tap): open the treadmill picker.
    function onSelect() as Boolean {
        if (!mBle.isConnected()) { return true; }
        WatchUi.pushView(new ScanView(mBle),
                         new ScanDelegate(),
                         WatchUi.SLIDE_LEFT);
        return true;
    }
}
