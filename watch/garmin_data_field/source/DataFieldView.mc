import Toybox.Activity;
import Toybox.Application;
import Toybox.Graphics;
import Toybox.Lang;
import Toybox.WatchUi;
import Toybox.System;

// Send-only data field. It no longer decides belt speed: it packs the *raw*
// current workout step (target/duration/intensity), the next step, how long
// the current one has left, and the activity timer state into a small binary
// frame and writes it to the bridge's telemetry characteristic. The bridge
// (core/workout_ctrl.c) resolves the targets, decides whether to command the
// next step early, dedups, keeps a slow keepalive, and drives the treadmill.
//
// Frame layout must match workout_ctrl.h (little-endian, 20 bytes, v2):
//   [0]      version (2)
//   [1]      timerState
//   [2]      flags (bit0 = a step is present; bits 1-4 are diagnostics,
//                  see FLAG_SRC_* below — the bridge ignores them;
//                  bit5 = the next step resolved, [15..18] are valid;
//                  bit6 = pre-roll speed increases only)
//   [3]      intensity
//   [4]      targetType (0 = speed)
//   [5..6]   targetLow  (mm/s for a speed target)
//   [7..8]   targetHigh (mm/s for a speed target)
//   [9]      durationType
//   [10..11] durationValue (u16; was a u32 in v1)
//   [12..13] remaining_s (0xFFFF unknown, 0xFFFE "far")
//   [14]     repetitionNumber
//   [15]     nextIntensity   (0xFF = none)
//   [16]     nextTargetType  (0xFF = none)
//   [17..18] nextTarget      (mm/s, already resolved to the range's midpoint)
//   [19]     advance_s       (0 = the bridge commands at the boundary, as v1)
class DataFieldView extends WatchUi.DataField {
    hidden const FRAME_VERSION = 2;
    hidden const FRAME_LEN = 20;

    // flags bits (byte [2]). bit0 is the wire contract: a speed step is
    // present. Bits 1-3 are diagnostics that say *why* a frame carries no
    // step. workout_ctrl.c only reads bit0, bit5 and bit6, so they are
    // backward-compatible; they exist to disambiguate the three ways
    // _packFrame can emit an all-sentinel frame, which are indistinguishable
    // on the wire otherwise.
    hidden const FLAG_HAS_STEP    = 0x01;
    hidden const FLAG_SRC_CATCH   = 0x02; // exception thrown while packing
    hidden const FLAG_SRC_NULL    = 0x04; // both step accessors returned null
    hidden const FLAG_SRC_NOTGT   = 0x08; // step resolved, but targetType missing
    hidden const FLAG_SRC_DUR     = 0x10; // throw was in the duration-value region
    hidden const FLAG_HAS_NEXT    = 0x20; // [15..18] carry the next step
    hidden const FLAG_ADV_UP_ONLY = 0x40; // pre-roll only speed increases

    // remaining_s sentinels. "far" is not ignorance: it means the boundary is
    // outside the window the bridge could act on, so there is no reason to
    // spend a 1 Hz write stream counting down to it.
    hidden const REMAIN_UNKNOWN = 0xFFFF;
    hidden const REMAIN_FAR     = 0xFFFE;
    hidden const REMAIN_MAX     = 0xFFFD;
    // Seconds of slack above advance_s inside which the exact value is sent,
    // so the bridge has the countdown in hand before it needs to act on it.
    hidden const REMAIN_WINDOW_S = 3;

    // Must match ADVANCE_MAX_S in core/workout_ctrl.c, which clamps again.
    hidden const ADVANCE_MAX     = 30;
    hidden const ADVANCE_DEFAULT = 5;

    // A boundary callback this recent is treated as having captured the step
    // start already, so the frame-diff fallback below does not overwrite it
    // with a later (and worse) estimate.
    hidden const BOUNDARY_FRESH_MS = 2000;

    // ---- ASSUMPTION, PENDING PHASE 0 MEASUREMENT --------------------------
    // The Connect IQ docs do not state what unit WorkoutStep.durationValue
    // uses. These two divisors turn it into the units the remaining-time maths
    // wants: seconds for a TIME step, metres for a DISTANCE one. They are set
    // for "already seconds" and "already metres". Phase 0 of
    // docs/superpowers/plans/2026-09-18-speed-advance.md must confirm both
    // against a workout with a known 60 s step and a known 400 m step; if
    // durationValue turns out to be milliseconds, DUR_TIME_DIV becomes 1000
    // and nothing else moves. See watch/README.md.
    hidden const DUR_TIME_DIV = 1;
    hidden const DUR_DIST_DIV = 1;
    // A treadmill's reported speed can sit at 0 between samples; dividing a
    // remaining distance by it would give nonsense, so floor it at a crawl.
    hidden const MIN_SPEED_MPS = 0.5f;

    hidden var mBle as CtrlBleDelegate or Null;
    hidden var mLastFrame as ByteArray or Null; // last frame actually sent
    hidden var mLinkGen as Number;              // link mLastFrame was sent on
    hidden var mTargetKmh as Float;             // resolved speed target, for display
    hidden var mHasSpeedTarget as Boolean;
    hidden var mTimerState as Number;
    hidden var mLastInfo as Activity.Info or Null;  // cached from compute() for fallback

    // Remaining-time bookkeeping. The step start is what Connect IQ does not
    // expose, so it is reconstructed from the boundary callbacks and, failing
    // those, from the frame itself changing shape.
    hidden var mStepStartTimerMs as Number or Null;  // timerTime when the step began
    hidden var mStepStartDistM as Float or Null;     // elapsedDistance when it began
    hidden var mStepKey as ByteArray or Null;        // current-slot bytes of the last frame
    hidden var mLastBoundaryMs as Number or Null;    // timerTime of the last boundary seen

    // Settings, re-read on every change from the phone (see DataFieldApp).
    hidden var mAdvanceSec as Number;
    hidden var mAdvanceUpOnly as Boolean;

    // Display state for the middle row, filled in by _packFrame.
    hidden var mHasStep as Boolean;             // a workout step resolved at all
    hidden var mNextLabel as String or Null;    // null = no next step
    hidden var mRemainDisplay as Number or Null;

    function initialize(ble as CtrlBleDelegate or Null) {
        DataField.initialize();
        mBle = ble;
        mLastFrame = null;
        mLinkGen = -1;
        mTargetKmh = 0.0f;
        mHasSpeedTarget = false;
        mTimerState = Activity.TIMER_STATE_OFF;
        mLastInfo = null;
        mStepStartTimerMs = null;
        mStepStartDistM = null;
        mStepKey = null;
        mLastBoundaryMs = null;
        mHasStep = false;
        mNextLabel = null;
        mRemainDisplay = null;
        mAdvanceSec = ADVANCE_DEFAULT;
        mAdvanceUpOnly = false;
        loadSettings();
    }

    function onLayout(dc as Dc) as Void {
    }

    // --- settings ---

    // Called from initialize() and from AppBase.onSettingsChanged(), so a
    // change made in the Connect IQ phone app lands without restarting the
    // activity: the new values go into byte 19 and flag bit 6 of the next
    // frame, which the change gate then sends by itself.
    function loadSettings() as Void {
        var sec = ADVANCE_DEFAULT;
        var upOnly = false;
        try {
            // getValue() returns a union, so narrow with instanceof rather
            // than trusting the store to hold the type settings.xml asked for.
            var v = Application.Properties.getValue("advanceSec");
            if (v instanceof Lang.Number) {
                sec = v;
            } else if (v instanceof Lang.Float) {
                sec = v.toNumber();
            }
            var u = Application.Properties.getValue("advanceUpOnly");
            if (u instanceof Lang.Boolean) { upOnly = u; }
        } catch (e) {
            // A build without the properties resource, or an unreadable store.
            // The defaults are a working configuration, so log and carry on
            // rather than leaving the field dead.
            System.println("DataFieldView settings error: " + e.getErrorMessage());
        }
        if (sec < 0) { sec = 0; }
        if (sec > ADVANCE_MAX) { sec = ADVANCE_MAX; }
        mAdvanceSec = sec;
        mAdvanceUpOnly = upOnly;
    }

    // --- frame encoding helpers ---

    hidden function _u16(f as ByteArray, off as Number, v as Number) as Void {
        f[off]     = v & 0xFF;
        f[off + 1] = (v >> 8) & 0xFF;
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

    // mm/s midpoint of a target range, tolerating a one-sided target. Exactly
    // what resolve_speed() does in core/workout_ctrl.c.
    hidden function _midpoint(low as Number, high as Number) as Number {
        if (low > 0 && high > 0) { return (low + high) / 2; }
        return low > 0 ? low : high;
    }

    // No-step frame: version + timer state, sentinel 0xFF for the
    // intensity/targetType/durationType fields and for both next-step fields,
    // remaining unknown, flags clear.
    hidden function _newBaseFrame(timerState as Number) as ByteArray {
        var f = new [FRAME_LEN]b;
        for (var i = 0; i < FRAME_LEN; i++) { f[i] = 0; }
        f[0] = FRAME_VERSION;
        f[1] = timerState & 0xFF;
        f[3] = 0xFF;  // intensity: invalid until known
        f[4] = 0xFF;  // targetType: invalid until known
        f[9] = 0xFF;  // durationType: invalid until known
        _u16(f, 12, REMAIN_UNKNOWN);
        f[15] = 0xFF; // nextIntensity: no next step until proven otherwise
        f[16] = 0xFF; // nextTargetType
        return f;
    }

    // --- step timing ---

    // The freshest Activity.Info available. compute() caches one every second;
    // the boundary callbacks fire between computes, where that cache can be up
    // to a second stale and the step start is exactly what we are trying to
    // pin down.
    hidden function _info() as Activity.Info or Null {
        var info as Activity.Info or Null = null;
        try {
            info = Activity.getActivityInfo();
        } catch (e) {
            info = null;
        }
        return info != null ? info : mLastInfo;
    }

    hidden function _timerMs() as Number or Null {
        var info = _info();
        if (info == null) { return null; }
        if (!(info has :timerTime) || info.timerTime == null) { return null; }
        return info.timerTime.toNumber();
    }

    // Record "the current step starts now". timerTime excludes paused time, so
    // a pause in the middle of a step needs no special handling: the clock
    // this is measured against stops with it.
    hidden function _markStepStart() as Void {
        var info = _info();
        var t as Number or Null = null;
        var d as Float or Null = null;
        if (info != null) {
            if ((info has :timerTime) && info.timerTime != null) {
                t = info.timerTime.toNumber();
            }
            if ((info has :elapsedDistance) && info.elapsedDistance != null) {
                d = info.elapsedDistance.toFloat();
            }
        }
        mStepStartTimerMs = t;
        mStepStartDistM = d;
        mLastBoundaryMs = t;
    }

    // Round up to whole seconds and clamp into the wire's range. Written
    // without Toybox.Math so the module does not have to be pulled in for one
    // ceiling: toNumber() truncates toward zero, which is floor for x >= 0.
    hidden function _ceilSeconds(x as Float) as Number {
        if (x <= 0.0f) { return 0; }
        var n = x.toNumber();
        if (x > n) { n += 1; }
        if (n > REMAIN_MAX) { n = REMAIN_MAX; }
        return n;
    }

    // Seconds left in the current step, or null when it cannot be known: no
    // recorded step start (the field was added mid-step, or the watch has not
    // fired a boundary yet), or a duration the watch cannot predict the end of
    // (lap button, HR, calories).
    hidden function _remainingS(durType as Number, durValue as Number) as Number or Null {
        // Nullable members are copied into locals before use: the Monkey C
        // type checker narrows a local across a null test, but not a member.
        var startMs = mStepStartTimerMs;
        if (startMs == null || durValue <= 0) { return null; }

        if (durType == Activity.WORKOUT_STEP_DURATION_TIME) {
            var now = _timerMs();
            if (now == null) { return null; }
            var total = durValue / DUR_TIME_DIV;
            var elapsed = (now - startMs) / 1000.0f;
            return _ceilSeconds(total - elapsed);
        }

        if (durType == Activity.WORKOUT_STEP_DURATION_DISTANCE) {
            var startM = mStepStartDistM;
            if (startM == null) { return null; }
            var info = _info();
            if (info == null) { return null; }
            if (!(info has :elapsedDistance) || info.elapsedDistance == null) {
                return null;
            }
            var gone = info.elapsedDistance.toFloat() - startM;
            var left = (durValue / DUR_DIST_DIV) - gone;
            // On a treadmill currentSpeed is the bridge's own SDM broadcast:
            // good enough for a one-second lead, not for anything finer.
            var v = MIN_SPEED_MPS;
            if ((info has :currentSpeed) && info.currentSpeed != null
                    && info.currentSpeed > MIN_SPEED_MPS) {
                v = info.currentSpeed.toFloat();
            }
            return _ceilSeconds(left / v);
        }

        return null;
    }

    // The bytes that identify *which* step a frame describes: the whole
    // current slot except the timer state and the flags. Two different steps
    // that are byte-identical here are indistinguishable to the bridge too, so
    // treating them as the same step costs nothing.
    hidden function _stepKey(f as ByteArray) as ByteArray {
        var k = new [10]b;
        for (var i = 0; i < 9; i++) { k[i] = f[3 + i]; }
        k[9] = f[14];
        return k;
    }

    // --- frame packing ---

    // Fill bytes 15-18 and set FLAG_HAS_NEXT from Activity.getNextWorkoutStep().
    //
    // Its own try/catch: a throw here must degrade the frame to "no next step"
    // — which is exactly v1 behaviour, and safe — rather than costing the
    // current step's target, which is the main product path.
    hidden function _packNext(f as ByteArray) as Void {
        try {
            var nStep = Activity.getNextWorkoutStep();
            if (nStep == null) { return; }

            var intensity = 0xFF;
            if ((nStep has :intensity) && nStep.intensity != null) {
                intensity = nStep.intensity & 0xFF;
            }
            // Drill through to the inner WorkoutStep when available, exactly
            // as the current slot does.
            if ((nStep has :step) && nStep.step != null) {
                nStep = nStep.step;
            }

            var tt = 0xFF;
            if ((nStep has :targetType) && nStep.targetType != null) {
                tt = nStep.targetType & 0xFF;
            }

            // Pre-resolved to a midpoint: the byte budget does not stretch to
            // two more u16s, and the midpoint is the only thing the bridge
            // ever does with a range.
            var mmps = 0;
            if (tt == Activity.WORKOUT_STEP_TARGET_SPEED) {
                var low = 0;
                var high = 0;
                if ((nStep has :targetValueLow) && nStep.targetValueLow != null) {
                    low = _targetToWire(tt, nStep.targetValueLow);
                }
                if ((nStep has :targetValueHigh) && nStep.targetValueHigh != null) {
                    high = _targetToWire(tt, nStep.targetValueHigh);
                }
                mmps = _midpoint(low, high);
            }

            f[2] = f[2] | FLAG_HAS_NEXT;
            f[15] = intensity;
            f[16] = tt;
            _u16(f, 17, mmps);

            if (mmps > 0) {
                mNextLabel = (mmps * 0.0036f).format("%.1f");
            } else if (intensity == Activity.WORKOUT_INTENSITY_REST) {
                // No speed of its own: the bridge answers a rest step with its
                // walk speed, so that is what the belt will actually do.
                mNextLabel = "walk";
            } else {
                mNextLabel = "--";
            }
        } catch (e) {
            System.println("DataFieldView _packNext error: " + e.getErrorMessage());
        }
    }

    // Build the 20-byte telemetry frame for the given timer state.
    // Tries module-level getCurrentWorkoutStep() first; falls back to
    // info.currentWorkoutStep from the compute() callback if the module
    // function returns null (seen on some CIQ System 8 builds).
    hidden function _packFrame(timerState as Number) as ByteArray {
        var f = _newBaseFrame(timerState);

        mHasSpeedTarget = false;
        mTargetKmh = 0.0f;
        mHasStep = false;
        mNextLabel = null;
        mRemainDisplay = null;

        try {
            var wStep = Activity.getCurrentWorkoutStep();
            // Fallback: try the Activity.Info field from the last compute() call.
            if (wStep == null && mLastInfo != null) {
                wStep = (mLastInfo has :currentWorkoutStep)
                    ? mLastInfo.currentWorkoutStep : null;
            }
            if (wStep == null) {
                f[2] = f[2] | FLAG_SRC_NULL;
            } else {
                mHasStep = true;
                if ((wStep has :intensity) && wStep.intensity != null) {
                    f[3] = wStep.intensity & 0xFF;
                }
                // Drill through to the inner WorkoutStep when available.
                if ((wStep has :step) && wStep.step != null) {
                    wStep = wStep.step;
                }

                // Duration first, before the target-type gate. v1 only packed
                // it on speed steps, which meant a rest step — which arrives
                // with an OPEN target — carried no duration at all and so
                // could never produce a remaining time. The bridge now reads
                // durationType for its short-step guard, and the countdown out
                // of a rest step is half the point of the advance.
                if ((wStep has :durationType) && wStep.durationType != null) {
                    f[9] = wStep.durationType & 0xFF;
                }
                // durationValue can come back as a Long on some steps; coerce
                // to Number so the byte writes cannot hit an implicit
                // Long->Byte conversion (suspected throw point, see the
                // bring-up log). Narrowed from u32 to u16 in v2: no step in a
                // treadmill workout needs more than 65535 s or 65535 m.
                try {
                    var dv = ((wStep has :durationValue) && wStep.durationValue != null)
                        ? wStep.durationValue.toNumber() : 0;
                    if (dv < 0) { dv = 0; }
                    if (dv > 65535) { dv = 65535; }
                    _u16(f, 10, dv);
                } catch (e2) {
                    f[2] = f[2] | FLAG_SRC_DUR;
                    throw e2;
                }

                var tt = (wStep has :targetType) ? wStep.targetType : null;
                if (tt == null) {
                    f[2] = f[2] | FLAG_SRC_NOTGT;
                } else {
                    f[4] = tt & 0xFF;

                    // Only a speed target maps to a belt command.
                    if (tt == Activity.WORKOUT_STEP_TARGET_SPEED) {
                        f[2] = f[2] | FLAG_HAS_STEP;

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

                        // Stash the resolved speed for the display.
                        var mmps = _midpoint(low, high);
                        if (mmps > 0) {
                            mHasSpeedTarget = true;
                            mTargetKmh = mmps * 0.0036f;
                        }
                    }
                }
            }
        } catch (e) {
            System.println("DataFieldView _packFrame error: " + e.getErrorMessage());
            // Keep the partially-built frame and mark it: each field write is
            // atomic and f started as a well-formed base frame, so whatever
            // got populated before the throw is still trustworthy and tells
            // us where the throw happened.
            f[2] = f[2] | FLAG_SRC_CATCH;
            mHasSpeedTarget = false;
            mTargetKmh = 0.0f;
        }

        // --- step-start bookkeeping ------------------------------------
        // Third and last way the step start is captured, after
        // onWorkoutStepComplete() and onTimerStart(): the frame changed shape
        // and no boundary callback owned up to it within the last couple of
        // seconds. The existing comment on onWorkoutStepComplete explains why
        // that happens — the step machine can lag the callback — and a start
        // recovered here is at most one compute() period late.
        var key = _stepKey(f);
        if (mStepKey == null) {
            // First frame of the session. The step is already under way and
            // its start is unknowable, so leave it null: remaining stays
            // "unknown" until a boundary is actually observed.
            mStepKey = key;
        } else if (!_bytesEqual(key, mStepKey)) {
            var now = _timerMs();
            var last = mLastBoundaryMs;
            var fresh = (last != null) && (now != null)
                && ((now - last) <= BOUNDARY_FRESH_MS);
            if (!fresh) { _markStepStart(); }
            mStepKey = key;
        }

        // --- next step and remaining time ------------------------------
        if (mHasStep) {
            _packNext(f);
            mRemainDisplay = _remainingS(f[9], f[10] | (f[11] << 8));
        }

        // Send gating: remaining_s changes every second, and the frame is
        // change-gated, so putting the exact value on the wire all step long
        // would turn a quiet timed step into a 1 Hz write stream. Only the
        // window the bridge can actually act on carries a real number;
        // outside it the field says "far", which is constant and so silent.
        var rem = mRemainDisplay;
        if (rem != null && mAdvanceSec > 0
                && rem <= mAdvanceSec + REMAIN_WINDOW_S) {
            _u16(f, 12, rem);
        } else if (mHasStep) {
            _u16(f, 12, REMAIN_FAR);
        }

        f[19] = mAdvanceSec & 0xFF;
        if (mAdvanceUpOnly) { f[2] = f[2] | FLAG_ADV_UP_ONLY; }
        return f;
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
    // It is also the primary source of the step start the remaining-time maths
    // needs, which is why _markStepStart() runs before the frame is packed:
    // this is the one moment the watch knows a step began, and it is recorded
    // whether or not the step machine has caught up enough to change the frame.
    //
    // Deliberately change-gated (force=false), NOT a _forcePush: if the workout
    // step machine has not advanced yet when this fires, _packFrame() still
    // returns the *old* step. Forcing would spend a BLE write re-sending it and
    // latch it as sent, which the next compute() then has to undo. Gating means
    // the worst case is silence here and the old 1 Hz behaviour takes over — so
    // this can only help, never hurt.
    function onWorkoutStepComplete() as Void {
        _markStepStart();
        _maybeSend(_packFrame(mTimerState), false);
    }

    // Timer transitions push immediately so pause stops the belt and resume
    // re-commands without waiting for the next 1 Hz compute().
    //
    // Only onTimerStart re-marks the step start: the activity starting is the
    // first step starting. A pause/resume must NOT, because timerTime excludes
    // paused time, so the clock the step start is measured against already
    // stops and restarts with the activity.
    function onTimerStart() as Void  {
        _markStepStart();
        _forcePush(Activity.TIMER_STATE_ON);
    }
    function onTimerResume() as Void { _forcePush(Activity.TIMER_STATE_ON); }
    function onTimerPause() as Void  { _forcePush(Activity.TIMER_STATE_PAUSED); }
    function onTimerStop() as Void   { _forcePush(Activity.TIMER_STATE_STOPPED); }

    hidden function _timerStr() as String {
        if (mTimerState == Activity.TIMER_STATE_ON) { return "RUN"; }
        if (mTimerState == Activity.TIMER_STATE_PAUSED) { return "PAUSE"; }
        if (mTimerState == Activity.TIMER_STATE_STOPPED) { return "STOP"; }
        return "OFF";
    }

    hidden function _mmss(secs as Number) as String {
        var m = secs / 60;
        var s = secs % 60;
        return m.format("%d") + ":" + s.format("%02d");
    }

    // Middle row: what the belt is about to be told to do, and when.
    //   "NEXT 12.0 in 0:45"  next step's pace, with the countdown
    //   "NEXT walk in 0:12"  a rest step, which the bridge answers with its
    //                        walk speed
    //   "NEXT 12.0"          remaining time not known (no boundary seen yet,
    //                        or a duration the watch cannot predict)
    //   "LAST STEP"          a step is running but there is no next one
    //   ""                   free run — nothing structured, and per
    //                        CLAUDE.md the belt is deliberately not touched
    hidden function _nextStr() as String {
        var label = mNextLabel;
        if (label == null) {
            // A step is running but nothing follows it, versus no structured
            // step at all. The second is a free run, which the bridge
            // deliberately does not act on, so the row stays empty.
            return mHasStep ? "LAST STEP" : "";
        }
        var rem = mRemainDisplay;
        if (rem == null) {
            return "NEXT " + label;
        }
        return "NEXT " + label + " in " + _mmss(rem);
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

        // Target speed + timer state (top). The timer state moved up here from
        // the middle row to make room for the next step, and it reads better
        // next to the speed it qualifies: "8.5 km/h RUN".
        var tgt = mHasSpeedTarget ? (mTargetKmh.format("%.1f") + " km/h") : "-- km/h";
        dc.drawText(w / 2, h / 4, Graphics.FONT_MEDIUM, tgt + " " + _timerStr(),
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);

        // Next step + countdown (middle).
        dc.drawText(w / 2, h / 2, Graphics.FONT_XTINY, _nextStr(),
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
