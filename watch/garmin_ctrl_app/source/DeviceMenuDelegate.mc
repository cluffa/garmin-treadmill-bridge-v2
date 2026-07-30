import Toybox.Lang;
import Toybox.WatchUi;

// Treadmill list: selecting an entry tells the bridge to switch to it
// ("CONNECT <idx>"); the bridge persists the pick as its new last-connected
// and the status screen updates from the pushed 'S' frame.
class DeviceMenuDelegate extends WatchUi.Menu2InputDelegate {
    hidden var mBle as BridgeBle;

    function initialize(ble as BridgeBle) {
        Menu2InputDelegate.initialize();
        mBle = ble;
    }

    function onSelect(item as WatchUi.MenuItem) as Void {
        var id = item.getId();
        if (id instanceof Lang.Number) {
            mBle.connectIndex(id as Lang.Number);
        }
        WatchUi.popView(WatchUi.SLIDE_RIGHT);   // back to the status screen
    }

    function onBack() as Void {
        WatchUi.popView(WatchUi.SLIDE_RIGHT);
    }
}
