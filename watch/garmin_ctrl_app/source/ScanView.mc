import Toybox.Graphics;
import Toybox.Lang;
import Toybox.Timer;
import Toybox.WatchUi;

// Picker flow driver: tells the bridge to SCAN, waits for the collect
// window so weak/slow treadmills make the list, then LISTs and shows the
// results as a menu. A 1 s tick drives the state machine and retries
// writes the BLE layer dropped (one write in flight at a time).
class ScanView extends WatchUi.View {
    const SCAN_WINDOW_SECS = 7;   // bridge pick window is 6 s

    hidden var mBle as BridgeBle;
    hidden var mTimer as Timer.Timer or Null;
    hidden var mTicks as Number;
    hidden var mScanSent as Boolean;
    hidden var mListSent as Boolean;
    hidden var mDone as Boolean;

    function initialize(ble as BridgeBle) {
        View.initialize();
        mBle = ble;
        mTimer = null;
        mTicks = 0;
        mScanSent = false;
        mListSent = false;
        mDone = false;
    }

    function onShow() as Void {
        mTicks = 0;
        mScanSent = false;
        mListSent = false;
        mDone = false;
        mTimer = new Timer.Timer();
        (mTimer as Timer.Timer).start(method(:onTick), 1000, true);
        mScanSent = mBle.requestScan();
    }

    function onHide() as Void {
        if (mTimer != null) {
            (mTimer as Timer.Timer).stop();
            mTimer = null;
        }
    }

    function onTick() as Void {
        if (mDone) { return; }
        mTicks++;

        if (!mScanSent) {                       // retry dropped SCAN write
            mScanSent = mBle.requestScan();
        } else if (mTicks >= SCAN_WINDOW_SECS && !mListSent) {
            mListSent = mBle.requestList();     // retried next tick if false
        } else if (mListSent && mBle.listComplete) {
            mDone = true;
            showMenu();
            return;
        } else if (mTicks > SCAN_WINDOW_SECS + 10) {
            mDone = true;                       // no 'E' frame — give up
            WatchUi.popView(WatchUi.SLIDE_RIGHT);
            return;
        }
        WatchUi.requestUpdate();
    }

    hidden function showMenu() as Void {
        var menu = new WatchUi.Menu2({:title => "Treadmills"});
        var devs = mBle.devices;
        for (var i = 0; i < devs.size(); i++) {
            var d = devs[i];
            var marks = "";
            if (d[:connected]) { marks += " ●"; }
            if (d[:saved])     { marks += " ★"; }
            menu.addItem(new WatchUi.MenuItem(
                (d[:name] as String) + marks,
                (d[:proto] as String) + "  " + (d[:rssi] as Number).toString() + " dBm",
                d[:idx],
                {}));
        }
        if (devs.size() == 0) {
            menu.addItem(new WatchUi.MenuItem("No treadmills found", "back to rescan",
                                              :none, {}));
        }
        // Replace the wait screen with the menu so Back returns to Status.
        WatchUi.switchToView(menu, new DeviceMenuDelegate(mBle), WatchUi.SLIDE_LEFT);
    }

    function onUpdate(dc as Dc) as Void {
        dc.setColor(Graphics.COLOR_WHITE, Graphics.COLOR_BLACK);
        dc.clear();
        var w = dc.getWidth();
        var h = dc.getHeight();
        var remain = SCAN_WINDOW_SECS - mTicks;
        var msg = remain > 0 ? "Scanning… " + remain.toString()
                             : "Fetching list…";
        dc.drawText(w / 2, h / 2, Graphics.FONT_SMALL, msg,
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);
        dc.drawText(w / 2, h * 3 / 4, Graphics.FONT_XTINY,
            mBle.devices.size().toString() + " found",
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);
    }
}

// Back during the scan wait just pops to the status screen.
class ScanDelegate extends WatchUi.BehaviorDelegate {
    function initialize() {
        BehaviorDelegate.initialize();
    }
}
