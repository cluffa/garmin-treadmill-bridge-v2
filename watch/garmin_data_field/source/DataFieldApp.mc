import Toybox.Application;
import Toybox.BluetoothLowEnergy;
import Toybox.Lang;
import Toybox.WatchUi;
import Toybox.System;

class DataFieldApp extends Application.AppBase {
    var mView as DataFieldView or Null;
    var mBle as CtrlBleDelegate or Null;

    function initialize() {
        AppBase.initialize();
        // Created here (not onStart) because some watches call
        // getInitialView() before onStart(); the constructor always runs
        // first, so the view gets a live delegate either way.
        mBle = new CtrlBleDelegate();
    }

    function onStart(state as Dictionary?) as Void {
        if (mBle != null) {
            BluetoothLowEnergy.setDelegate(mBle);
            mBle.registerProfile();   // scan starts from onProfileRegister
        }
    }

    function onStop(state as Dictionary?) as Void {
        if (mBle != null) {
            mBle.shutdown();
        }
    }

    function getInitialView() {
        mView = new DataFieldView(mBle);
        return [ mView ];
    }

    // The Connect IQ phone app can change advanceSec/advanceUpOnly mid-activity.
    // Re-read them into the view rather than restarting anything: they go into
    // every frame, so the change gate sends the new values by itself.
    function onSettingsChanged() as Void {
        if (mView != null) {
            mView.loadSettings();
        }
    }
}
