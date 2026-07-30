import Toybox.BluetoothLowEnergy;
import Toybox.Lang;
import Toybox.StringUtil;
import Toybox.System;
import Toybox.WatchUi;

// BLE client for the nRF52840 bridge's control service ("TMILL-CTRL").
//
// Contract with boards/xiao-nrf52840/platform_ble_ctrl_svc.c:
//  - control char  (0x0002): ASCII ctrl_dispatch lines ("SCAN", "CONNECT 2")
//  - response char (0x0003): <=20-byte binary frames (ctrl_frames.h)
//      'D' idx rssi proto flags name\0   scan-list entry   (reply to LIST)
//      'E' count                         end of list       (reply to LIST)
//      'S' connected proto name\0        treadmill status  (reply to STATUS,
//                                        also pushed on link change)
class BridgeBle extends BluetoothLowEnergy.BleDelegate {
    const CTRL_SVC_UUID = BluetoothLowEnergy.stringToUuid("A6ED0001-D344-460A-8075-B9E8EC90D71B");
    const CTRL_CHR_UUID = BluetoothLowEnergy.stringToUuid("A6ED0002-D344-460A-8075-B9E8EC90D71B");
    const RSP_CHR_UUID  = BluetoothLowEnergy.stringToUuid("A6ED0003-D344-460A-8075-B9E8EC90D71B");

    const PROTO_NAMES = ["FTMS", "iFit"];

    hidden var mDevice as BluetoothLowEnergy.Device or Null;
    hidden var mScanning as Boolean;
    hidden var mSubscribed as Boolean;
    hidden var mWritePending as Boolean;

    // Latest LIST results: array of dicts
    // {:idx, :rssi, :proto, :connected, :saved, :name}
    var devices as Array<Dictionary>;
    var listComplete as Boolean;

    // Latest treadmill status from an 'S' frame.
    var tmConnected as Boolean;
    var tmName as String;
    var tmProto as String;

    function initialize() {
        BleDelegate.initialize();
        mDevice = null;
        mScanning = false;
        mSubscribed = false;
        mWritePending = false;
        devices = [] as Array<Dictionary>;
        listComplete = false;
        tmConnected = false;
        tmName = "";
        tmProto = "";
    }

    // ---- connection to the bridge ------------------------------------------

    function registerProfile() as Void {
        var profile = {
            :uuid => CTRL_SVC_UUID,
            :characteristics => [{
                :uuid => CTRL_CHR_UUID
            }, {
                :uuid => RSP_CHR_UUID,
                :descriptors => [BluetoothLowEnergy.cccdUuid()]
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
        if (status == BluetoothLowEnergy.STATUS_SUCCESS) {
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
        WatchUi.requestUpdate();
    }

    hidden function advertisesCtrlSvc(r as BluetoothLowEnergy.ScanResult) as Boolean {
        var uuids = r.getServiceUuids();
        for (var u = uuids.next(); u != null; u = uuids.next()) {
            if (u.equals(CTRL_SVC_UUID)) { return true; }
        }
        return false;
    }

    function onScanResults(scanResults as BluetoothLowEnergy.Iterator) as Void {
        for (var r = scanResults.next(); r != null; r = scanResults.next()) {
            if (!(r instanceof BluetoothLowEnergy.ScanResult)) { continue; }
            if (advertisesCtrlSvc(r)) {
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
            subscribe();
        } else {
            if (mDevice != null) {
                try { BluetoothLowEnergy.unpairDevice(mDevice); } catch (e) {}
            }
            mDevice = null;
            mSubscribed = false;
            mWritePending = false;
            startScan();
        }
        WatchUi.requestUpdate();
    }

    // Enable notifications on the response characteristic; the bridge greets
    // with an 'S' status frame once the CCCD write lands.
    hidden function subscribe() as Void {
        if (mDevice == null) { return; }
        var svc = mDevice.getService(CTRL_SVC_UUID);
        if (svc == null) { return; }
        var ch = svc.getCharacteristic(RSP_CHR_UUID);
        if (ch == null) { return; }
        var cccd = ch.getDescriptor(BluetoothLowEnergy.cccdUuid());
        if (cccd == null) { return; }
        try {
            cccd.requestWrite([0x01, 0x00]b);
        } catch (e) {
            System.println("BLE cccd write failed: " + e.getErrorMessage());
        }
    }

    function onDescriptorWrite(descriptor as BluetoothLowEnergy.Descriptor,
                               status as BluetoothLowEnergy.Status) as Void {
        mSubscribed = (status == BluetoothLowEnergy.STATUS_SUCCESS);
        WatchUi.requestUpdate();
    }

    function shutdown() as Void {
        try {
            BluetoothLowEnergy.setScanState(BluetoothLowEnergy.SCAN_STATE_OFF);
            if (mDevice != null) { BluetoothLowEnergy.unpairDevice(mDevice); }
        } catch (e) {}
        mDevice = null;
    }

    function isConnected() as Boolean { return mDevice != null && mSubscribed; }
    function isScanning() as Boolean { return mScanning; }

    // ---- commands -----------------------------------------------------------

    hidden function strToBytes(s as String) as ByteArray {
        var chars = s.toUtf8Array();
        var ba = new [chars.size()]b;
        for (var i = 0; i < chars.size(); i++) { ba[i] = chars[i]; }
        return ba;
    }

    // One write in flight at a time (dropped commands are retried by the UI).
    function sendCommand(cmd as String) as Boolean {
        if (mDevice == null || mWritePending) { return false; }
        var svc = mDevice.getService(CTRL_SVC_UUID);
        if (svc == null) { return false; }
        var ch = svc.getCharacteristic(CTRL_CHR_UUID);
        if (ch == null) { return false; }
        try {
            ch.requestWrite(strToBytes(cmd),
                            {:writeType => BluetoothLowEnergy.WRITE_TYPE_DEFAULT});
            mWritePending = true;
            System.println("BLE tx: " + cmd);
            return true;
        } catch (e) {
            System.println("BLE write failed: " + e.getErrorMessage());
            return false;
        }
    }

    function onCharacteristicWrite(characteristic as BluetoothLowEnergy.Characteristic,
                                   status as BluetoothLowEnergy.Status) as Void {
        mWritePending = false;
    }

    function requestScan() as Boolean {
        devices = [] as Array<Dictionary>;
        listComplete = false;
        return sendCommand("SCAN");
    }

    function requestList() as Boolean {
        devices = [] as Array<Dictionary>;
        listComplete = false;
        return sendCommand("LIST");
    }

    function connectIndex(idx as Number) as Boolean {
        return sendCommand("CONNECT " + idx.toString());
    }

    // ---- response frames ----------------------------------------------------

    hidden function nameFromBytes(value as ByteArray, start as Number) as String {
        var chars = [] as Array<Number>;
        for (var i = start; i < value.size(); i++) {
            var b = value[i];
            if (b == 0) { break; }
            chars.add(b);
        }
        if (chars.size() == 0) { return "(unnamed)"; }
        var s = StringUtil.utf8ArrayToString(chars);
        return s != null ? s : "(unnamed)";
    }

    hidden function protoName(p as Number) as String {
        return (p >= 0 && p < PROTO_NAMES.size()) ? PROTO_NAMES[p] : "?";
    }

    function onCharacteristicChanged(characteristic as BluetoothLowEnergy.Characteristic,
                                     value as ByteArray) as Void {
        if (!characteristic.getUuid().equals(RSP_CHR_UUID) || value.size() < 2) {
            return;
        }
        var tag = value[0];
        if (tag == 0x44 /* 'D' */ && value.size() >= 6) {
            var rssi = value[2];
            if (rssi > 127) { rssi -= 256; }   // int8
            devices.add({
                :idx       => value[1],
                :rssi      => rssi,
                :proto     => protoName(value[3]),
                :connected => (value[4] & 0x01) != 0,
                :saved     => (value[4] & 0x02) != 0,
                :name      => nameFromBytes(value, 5),
            });
        } else if (tag == 0x45 /* 'E' */) {
            listComplete = true;
        } else if (tag == 0x53 /* 'S' */ && value.size() >= 3) {
            tmConnected = value[1] != 0;
            tmProto = protoName(value[2]);
            tmName = value.size() > 3 ? nameFromBytes(value, 3) : "";
        }
        WatchUi.requestUpdate();
    }
}
