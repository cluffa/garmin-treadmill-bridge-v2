import Toybox.Activity;
import Toybox.Graphics;
import Toybox.Lang;
import Toybox.WatchUi;
import Toybox.System;

// Send-only data field. It no longer decides belt speed: it packs the *raw*
// current workout step (target/duration/intensity) plus the activity timer
// state into a small binary frame and writes it to the bridge's telemetry
// characteristic. The bridge (components/bridge_core/workout_ctrl.c) resolves
// the target, dedups, keeps a slow keepalive, and drives the treadmill.
//
// Frame layout must match workout_ctrl.h (little-endian, 15 bytes):
//   [0]      version (1)
//   [1]      timerState
//   [2]      flags (bit0 = a step is present; bits 1-4 are diagnostics,
//                  see FLAG_SRC_* below — the bridge ignores them)
//   [3]      intensity
//   [4]      targetType (0 = speed)
//   [5..6]   targetLow  (mm/s for a speed target)
//   [7..8]   targetHigh (mm/s for a speed target)
//   [9]      durationType
//   [10..13] durationValue
//   [14]     repetitionNumber
class DataFieldView extends WatchUi.DataField {
    hidden const FRAME_VERSION = 1;
    hidden const FRAME_LEN = 15;

    // flags bits (byte [2]). bit0 is the wire contract: a speed step is
    // present. Bits 1-3 are diagnostics that say *why* a frame carries no
    // step. workout_ctrl.c only reads bit0, so they are backward-compatible;
    // they exist to disambiguate the three ways _packFrame can emit an
    // all-sentinel frame, which are indistinguishable on the wire otherwise.
    hidden const FLAG_HAS_STEP  = 0x01;
    hidden const FLAG_SRC_CATCH = 0x02; // exception thrown while packing
    hidden const FLAG_SRC_NULL  = 0x04; // both step accessors returned null
    hidden const FLAG_SRC_NOTGT = 0x08; // step resolved, but targetType missing
    hidden const FLAG_SRC_DUR   = 0x10; // throw was in the duration-value region

    hidden var mBle as CtrlBleDelegate or Null;
    hidden var mLastFrame as ByteArray or Null; // last frame actually sent
    hidden var mLinkGen as Number;              // link mLastFrame was sent on
    hidden var mTargetKmh as Float;             // resolved speed target, for display
    hidden var mHasSpeedTarget as Boolean;
    hidden var mTimerState as Number;
    hidden var mLastInfo as Activity.Info or Null;  // cached from compute() for fallback

    function initialize(ble as CtrlBleDelegate or Null) {
        DataField.initialize();
        mBle = ble;
        mLastFrame = null;
        mLinkGen = -1;
        mTargetKmh = 0.0f;
        mHasSpeedTarget = false;
        mTimerState = Activity.TIMER_STATE_OFF;
        mLastInfo = null;
    }

    function onLayout(dc as Dc) as Void {
    }

    // --- frame encoding helpers ---

    hidden function _u16(f as ByteArray, off as Number, v as Number) as Void {
        f[off]     = v & 0xFF;
        f[off + 1] = (v >> 8) & 0xFF;
    }

    hidden function _u32(f as ByteArray, off as Number, v as Number) as Void {
        f[off]     = v & 0xFF;
        f[off + 1] = (v >> 8) & 0xFF;
        f[off + 2] = (v >> 16) & 0xFF;
        f[off + 3] = (v >> 24) & 0xFF;
    }

    // Encode a target value for the wire. Speed targets are m/s → mm/s; other
    // target types are packed raw (informational — the bridge only acts on
    // speed). Clamped to uint16.
    hidden function _targetToWire(targetType as Number, val) as Number {
        if (val == null) { return 0; }
        var n;
        if (targetType == Activity.WORKOUT_STEP_TARGET_SPEED) {
            n = (val.toFloat() * 1000.0f).toNumber();
        } else {
            n = val.toNumber();
        }
        if (n < 0) { n = 0; }
        if (n > 65535) { n = 65535; }
        return n;
    }

    // No-step frame: version + timer state, sentinel 0xFF for the
    // intensity/targetType/durationType fields, flags clear.
    hidden function _newBaseFrame(timerState as Number) as ByteArray {
        var f = new [FRAME_LEN]b;
        for (var i = 0; i < FRAME_LEN; i++) { f[i] = 0; }
        f[0] = FRAME_VERSION;
        f[1] = timerState & 0xFF;
        f[3] = 0xFF; // intensity: invalid until known
        f[4] = 0xFF; // targetType: invalid until known
        f[9] = 0xFF; // durationType: invalid until known
        return f;
    }

    // Build the 15-byte telemetry frame for the given timer state.
    // Tries module-level getCurrentWorkoutStep() first; falls back to
    // info.currentWorkoutStep from the compute() callback if the module
    // function returns null (seen on some CIQ System 8 builds).
    hidden function _packFrame(timerState as Number) as ByteArray {
        var f = _newBaseFrame(timerState);

        mHasSpeedTarget = false;
        mTargetKmh = 0.0f;

        try {
            var wStep = Activity.getCurrentWorkoutStep();
            // Fallback: try the Activity.Info field from the last compute() call.
            if (wStep == null && mLastInfo != null) {
                wStep = (mLastInfo has :currentWorkoutStep)
                    ? mLastInfo.currentWorkoutStep : null;
            }
            if (wStep == null) {
                f[2] = FLAG_SRC_NULL;
                return f;
            }
            if ((wStep has :intensity) && wStep.intensity != null) {
                f[3] = wStep.intensity & 0xFF;
            }
            // Drill through to the inner WorkoutStep when available.
            if ((wStep has :step) && wStep.step != null) {
                wStep = wStep.step;
            }

            if (!(wStep has :targetType)) {
                f[2] = FLAG_SRC_NOTGT;
                return f;
            }
            var tt = wStep.targetType;
            if (tt == null) {
                f[2] = FLAG_SRC_NOTGT;
                return f;
            }
            f[4] = tt & 0xFF;

            // Only a speed target maps to a belt command.
            if (tt != Activity.WORKOUT_STEP_TARGET_SPEED) { return f; }

            f[2] = 0x01; // has step

            var low  = 0;
            var high = 0;
            if ((wStep has :targetValueLow) && wStep.targetValueLow != null) {
                low = _targetToWire(tt, wStep.targetValueLow);
            }
            if ((wStep has :targetValueHigh) && wStep.targetValueHigh != null) {
                high = _targetToWire(tt, wStep.targetValueHigh);
            }
            _u16(f, 5, low);
            _u16(f, 7, high);

            if ((wStep has :durationType) && wStep.durationType != null) {
                f[9] = wStep.durationType & 0xFF;
            }
            // durationValue can come back as a Long on some steps; coerce to
            // Number so _u32's byte writes cannot hit an implicit Long->Byte
            // conversion (suspected throw point, see the bring-up log).
            try {
                var dv = ((wStep has :durationValue) && wStep.durationValue != null)
                    ? wStep.durationValue.toNumber() : 0;
                _u32(f, 10, dv);
            } catch (e2) {
                f[2] |= FLAG_SRC_DUR;
                throw e2;
            }

            // Stash the resolved speed for the display (mm/s midpoint -> km/h).
            var mmps = (low > 0 && high > 0) ? ((low + high) / 2) : (low > 0 ? low : high);
            if (mmps > 0) {
                mHasSpeedTarget = true;
                mTargetKmh = mmps * 0.0036f;
            }
            return f;
        } catch (e) {
            System.println("DataFieldView _packFrame error: " + e.getErrorMessage());
            // Keep the partially-built frame and mark it: each field write is
            // atomic and f started as a well-formed base frame, so whatever
            // got populated before the throw is still trustworthy and tells
            // us where the throw happened.
            f[2] |= FLAG_SRC_CATCH;
            mHasSpeedTarget = false;
            mTargetKmh = 0.0f;
            return f;
        }
    }

    hidden function _bytesEqual(a as ByteArray, b as ByteArray or Null) as Boolean {
        if (b == null || a.size() != b.size()) { return false; }
        for (var i = 0; i < a.size(); i++) {
            if (a[i] != b[i]) { return false; }
        }
        return true;
    }

    // Send the frame if it changed (or force it, for a timer transition). Only
    // latches it as sent when the write was accepted for delivery.  This is the
    // single catch boundary for the BLE send path, so the onTimer* and
    // onWorkoutStepComplete pushes are protected the same as the 1 Hz
    // compute() sends.
    hidden function _maybeSend(frame as ByteArray, force as Boolean) as Void {
        try {
            if (mBle == null || !mBle.isConnected()) { return; }
            // mLastFrame means "the bridge already has this" — only true of the
            // link it was sent on. After a reconnect the bridge is holding
            // whatever it had before with nothing in flight to correct it, so
            // drop the latch and let this frame through.
            var gen = mBle.linkGeneration();
            if (gen != mLinkGen) {
                mLastFrame = null;
                mLinkGen = gen;
            }
            if (!force && _bytesEqual(frame, mLastFrame)) { return; }
            if (mBle.writeWorkoutFrame(frame)) {
                mLastFrame = frame;
            }
        } catch (e) {
            System.println("DataFieldView send error: " + e.getErrorMessage());
        }
    }

    hidden function _forcePush(timerState as Number) as Void {
        mTimerState = timerState;
        _maybeSend(_packFrame(timerState), true);
    }

    // No try/catch needed here: _packFrame and _maybeSend each catch
    // internally, and the timerState read is has/null-guarded.
    function compute(info as Activity.Info) as Void {
        // Consume any deferred scan re-arm first: compute() is the plain timer
        // context the BLE delegate must not touch the stack outside of.
        if (mBle != null) {
            mBle.tick();
        }
        mLastInfo = info;
        mTimerState = (info has :timerState) && info.timerState != null
            ? info.timerState : Activity.TIMER_STATE_OFF;
        _maybeSend(_packFrame(mTimerState), false);
    }

    // A workout step boundary is the moment the belt speed needs to change, and
    // waiting for the next compute() to notice is the single largest term in
    // the end-to-end pace lag: the boundary lands at a uniformly random point
    // inside the 1 Hz compute period, so it costs ~0.5 s on average and up to a
    // full second. This callback fires *at* the boundary, so the new target
    // goes out as soon as the watch itself knows about it. Measured lag before
    // this existed: 2.26 s mean (test/pace_lag_report.py on the 2026-08-01
    // trace), of which ~0.5 s is the watch's own 1 Hz recording of the ANT
    // trace and never reaches the belt.
    //
    // Deliberately change-gated (force=false), NOT a _forcePush: if the workout
    // step machine has not advanced yet when this fires, _packFrame() still
    // returns the *old* step. Forcing would spend a BLE write re-sending it and
    // latch it as sent, which the next compute() then has to undo. Gating means
    // the worst case is silence here and the old 1 Hz behaviour takes over — so
    // this can only help, never hurt.
    function onWorkoutStepComplete() as Void {
        _maybeSend(_packFrame(mTimerState), false);
    }

    // Timer transitions push immediately so pause stops the belt and resume
    // re-commands without waiting for the next 1 Hz compute().
    function onTimerStart() as Void  { _forcePush(Activity.TIMER_STATE_ON); }
    function onTimerResume() as Void { _forcePush(Activity.TIMER_STATE_ON); }
    function onTimerPause() as Void  { _forcePush(Activity.TIMER_STATE_PAUSED); }
    function onTimerStop() as Void   { _forcePush(Activity.TIMER_STATE_STOPPED); }

    hidden function _timerStr() as String {
        if (mTimerState == Activity.TIMER_STATE_ON) { return "RUN"; }
        if (mTimerState == Activity.TIMER_STATE_PAUSED) { return "PAUSE"; }
        if (mTimerState == Activity.TIMER_STATE_STOPPED) { return "STOP"; }
        return "OFF";
    }

    function onUpdate(dc as Dc) as Void {
        var fgColor = Graphics.COLOR_BLACK;
        var bgColor = Graphics.COLOR_WHITE;
        if (getBackgroundColor() == Graphics.COLOR_BLACK) {
            fgColor = Graphics.COLOR_WHITE;
            bgColor = Graphics.COLOR_BLACK;
        }
        dc.setColor(fgColor, bgColor);
        dc.clear();

        var w = dc.getWidth();
        var h = dc.getHeight();

        // Target speed (top)
        var tgt = mHasSpeedTarget ? (mTargetKmh.format("%.1f") + " km/h") : "-- km/h";
        dc.drawText(w / 2, h / 4, Graphics.FONT_MEDIUM, tgt,
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);

        // Timer state (middle)
        dc.drawText(w / 2, h / 2, Graphics.FONT_XTINY, _timerStr(),
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);

        // Bridge link state + build stamp (bottom): "CONN 0803-1857".
        // The stamp is here rather than behind a menu because the question it
        // answers — "is the watch running the build I just sideloaded?" — comes
        // up mid-run, and a sideload that silently did not take looks exactly
        // like one that did. It stays visible when connected for the same
        // reason: a stale build that still connects is the confusing case.
        // CONN = link actually up; PAIR = pairDevice() issued but no CONNECTED
        // yet (the state the old display mislabelled CONN); SCAN = scanning.
        var link = "--";
        if (mBle != null) {
            if (mBle.isLinkUp()) {
                link = "CONN";
            } else if (mBle.isConnected()) {
                link = "PAIR";
            } else if (mBle.isScanning()) {
                link = "SCAN";
            }
        }
        dc.drawText(w / 2, h * 3 / 4, Graphics.FONT_XTINY,
            link + " " + BuildInfo.STAMP,
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);
    }
}
