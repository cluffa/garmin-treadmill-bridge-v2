import Toybox.BluetoothLowEnergy;
import Toybox.Lang;
import Toybox.System;

// BLE client for the nRF52840 bridge's control service ("TMILL-CTRL").
// Send-only: the field writes a raw workout-telemetry frame to the telemetry
// characteristic (…0004); the bridge's workout_ctrl module decides the belt
// speed. Treadmill speed reaches the watch natively over ANT+ (SDM footpod),
// so nothing is read back.
class CtrlBleDelegate extends BluetoothLowEnergy.BleDelegate {
    // Contract with boards/xiao-nrf52840/platform_ble_ctrl_svc.c
    const CTRL_SVC_UUID  = BluetoothLowEnergy.stringToUuid("A6ED0001-D344-460A-8075-B9E8EC90D71B");
    // Workout telemetry char (binary frames), decoded by bridge_core/workout_ctrl.c.
    const CTRL_WKT_UUID  = BluetoothLowEnergy.stringToUuid("A6ED0004-D344-460A-8075-B9E8EC90D71B");

    hidden var mDevice as BluetoothLowEnergy.Device or Null;
    hidden var mProfileRegistered as Boolean;
    hidden var mScanning as Boolean;
    hidden var mWritePending as Boolean;
    hidden var mQueued as ByteArray or Null;    // frame that arrived mid-write
    hidden var mLinkGen as Number;              // bumped on every (re)connect

    function initialize() {
        BleDelegate.initialize();
        mDevice = null;
        mProfileRegistered = false;
        mScanning = false;
        mWritePending = false;
        mQueued = null;
        mLinkGen = 0;
    }

    // Async; results arrive in onProfileRegister. Data fields get a single
    // BLE profile slot — this is it.
    function registerProfile() as Void {
        var profile = {
            :uuid => CTRL_SVC_UUID,
            :characteristics => [{
                :uuid => CTRL_WKT_UUID
            }]
        };
        try {
            BluetoothLowEnergy.registerProfile(profile);
        } catch (e) {
            System.println("BLE registerProfile failed: " + e.getErrorMessage());
        }
    }

    function onProfileRegister(uuid as BluetoothLowEnergy.Uuid,
                               status as BluetoothLowEnergy.Status) as Void {
        mProfileRegistered = (status == BluetoothLowEnergy.STATUS_SUCCESS);
        System.println("BLE profile register status=" + status);
        if (mProfileRegistered) {
            startScan();
        }
    }

    function startScan() as Void {
        if (mDevice != null || mScanning) { return; }
        try {
            BluetoothLowEnergy.setScanState(BluetoothLowEnergy.SCAN_STATE_SCANNING);
        } catch (e) {
            System.println("BLE scan start failed: " + e.getErrorMessage());
        }
    }

    function onScanStateChange(scanState as BluetoothLowEnergy.ScanState,
                               status as BluetoothLowEnergy.Status) as Void {
        mScanning = (scanState == BluetoothLowEnergy.SCAN_STATE_SCANNING);
    }

    hidden function advertisesCtrlSvc(r as BluetoothLowEnergy.ScanResult) as Boolean {
        var uuids = r.getServiceUuids();
        for (var u = uuids.next(); u != null; u = uuids.next()) {
            if (u.equals(CTRL_SVC_UUID)) {
                return true;
            }
        }
        return false;
    }

    function onScanResults(scanResults as BluetoothLowEnergy.Iterator) as Void {
        for (var r = scanResults.next(); r != null; r = scanResults.next()) {
            if (!(r instanceof BluetoothLowEnergy.ScanResult)) { continue; }
            if (advertisesCtrlSvc(r)) {
                System.println("BLE found TMILL-CTRL, pairing");
                BluetoothLowEnergy.setScanState(BluetoothLowEnergy.SCAN_STATE_OFF);
                try {
                    mDevice = BluetoothLowEnergy.pairDevice(r);
                } catch (e) {
                    System.println("BLE pair failed: " + e.getErrorMessage());
                    startScan();
                }
                return;
            }
        }
    }

    function onConnectedStateChanged(device as BluetoothLowEnergy.Device,
                                     state as BluetoothLowEnergy.ConnectionState) as Void {
        if (state == BluetoothLowEnergy.CONNECTION_STATE_CONNECTED) {
            mDevice = device;
            mWritePending = false;
            mQueued = null;
            mLinkGen++;
            System.println("BLE bridge connected");
        } else {
            System.println("BLE bridge disconnected — rescanning");
            if (mDevice != null) {
                try {
                    BluetoothLowEnergy.unpairDevice(mDevice);
                } catch (e) {
                    // already gone; nothing to clean up
                }
            }
            mDevice = null;
            mWritePending = false;
            mQueued = null;   // it can never go out on this link now
            startScan();
        }
    }

    // Release BLE resources when the activity ends.
    function shutdown() as Void {
        try {
            BluetoothLowEnergy.setScanState(BluetoothLowEnergy.SCAN_STATE_OFF);
            if (mDevice != null) {
                BluetoothLowEnergy.unpairDevice(mDevice);
            }
        } catch (e) {
        }
        mDevice = null;
    }

    function isConnected() as Boolean {
        return mDevice != null;
    }

    function isScanning() as Boolean {
        return mScanning;
    }

    // Increments on every (re)connect. The caller latches "the bridge already
    // has this frame"; that latch is only meaningful for the link the frame
    // went out on, so a change here tells it to re-send unconditionally.
    // Without it, a link that drops and comes back leaves the bridge holding
    // whatever it had before with no frame in flight to correct it — nothing
    // re-sends until the workout step next changes.
    function linkGeneration() as Number {
        return mLinkGen;
    }

    // Write a raw workout-telemetry frame to the bridge. One write in flight at
    // a time. A frame that arrives while a write is outstanding is *queued*
    // rather than dropped, and goes out from onCharacteristicWrite the instant
    // the link frees up — the caller's next chance would otherwise be its 1 Hz
    // compute(), which put a whole extra second of lag on the belt every time
    // a step boundary landed near an in-flight write.
    //
    // Only the newest frame is kept: an older one is by definition superseded,
    // and the bridge only cares about the current target.
    //
    // Returns true when the frame is accepted for delivery (issued or queued),
    // which is what the caller latches on.
    function writeWorkoutFrame(frame as ByteArray) as Boolean {
        if (mDevice == null) { return false; }
        if (mWritePending) {
            mQueued = frame;
            return true;
        }
        return _issue(frame);
    }

    hidden function _issue(frame as ByteArray) as Boolean {
        try {
            var svc = mDevice.getService(CTRL_SVC_UUID);
            if (svc == null) { return false; }
            var ch = svc.getCharacteristic(CTRL_WKT_UUID);
            if (ch == null) { return false; }
            ch.requestWrite(frame,
                            {:writeType => BluetoothLowEnergy.WRITE_TYPE_DEFAULT});
            mWritePending = true;
            return true;
        } catch (e) {
            System.println("BLE write failed: " + e.getErrorMessage());
            return false;
        }
    }

    function onCharacteristicWrite(characteristic as BluetoothLowEnergy.Characteristic,
                                   status as BluetoothLowEnergy.Status) as Void {
        mWritePending = false;
        if (status != BluetoothLowEnergy.STATUS_SUCCESS) {
            System.println("BLE write status=" + status);
        }
        // Flush whatever piled up behind this write. Clear the slot first, so a
        // failure here cannot leave a frame stuck in the queue forever; the
        // 1 Hz compute() re-send remains the backstop.
        var pending = mQueued;
        mQueued = null;
        if (pending != null && mDevice != null) {
            _issue(pending);
        }
    }
}
