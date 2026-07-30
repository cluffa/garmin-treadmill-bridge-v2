import Toybox.Application;
import Toybox.BluetoothLowEnergy;
import Toybox.Lang;
import Toybox.WatchUi;

// Treadmill picker / bridge companion app. Connects to the nRF52840 bridge
// ("TMILL-CTRL") and lets the user choose which treadmill the bridge links
// to; left alone, the bridge auto-connects (last-connected, else closest).
class CtrlApp extends Application.AppBase {
    var mBle as BridgeBle or Null;

    function initialize() {
        AppBase.initialize();
    }

    function onStart(state as Dictionary?) as Void {
        mBle = new BridgeBle();
        BluetoothLowEnergy.setDelegate(mBle);
        mBle.registerProfile();   // scan starts from onProfileRegister
    }

    function onStop(state as Dictionary?) as Void {
        if (mBle != null) {
            mBle.shutdown();
        }
    }

    function getInitialView() {
        var ble = mBle as BridgeBle;
        return [ new StatusView(ble), new StatusDelegate(ble) ];
    }
}
