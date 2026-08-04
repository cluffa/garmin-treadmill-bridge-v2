#!/bin/sh
# Sideload a built Connect IQ .prg onto the fenix 8 over MTP.
#
# The watch has no USB mass-storage mode (/Volumes/GARMIN never mounts), so this
# goes through libmtp (brew install libmtp). Like the flash targets it CONSUMES
# an existing build; it never invokes monkeyc. See watch/README.md.
#
#   tools/ciq_sideload.sh <project-dir>
#
# Env:
#   SIDELOAD_FORCE=1        send even if sources are newer than the .prg
#   SIDELOAD_CHECK_DUPES=1  run the (slow, hang-prone) duplicate-app.prg scan
#   SIDELOAD_LOOKUP_TIMEOUT seconds for folder-id lookup   (default 60)
#   SIDELOAD_SEND_TIMEOUT   seconds for the transfer        (default 240)
#
# Every MTP call is wrapped in a timeout. Garmin's MTP stack blocks
# indefinitely rather than erroring: a `mtp-files` scan was observed sitting at
# 0.0% CPU for four minutes with no transfer started. A hang that reports
# itself beats one that looks like slow progress.

PRJ="$1"
[ -n "$PRJ" ] || { echo "usage: $0 <project-dir>" >&2; exit 2; }

PRG="$PRJ/out/app.prg"
LOOKUP_TIMEOUT="${SIDELOAD_LOOKUP_TIMEOUT:-60}"
SEND_TIMEOUT="${SIDELOAD_SEND_TIMEOUT:-240}"

TMP="${TMPDIR:-/tmp}/ciq-sideload.$$"
trap 'rm -f "$TMP"' EXIT INT TERM

# run_to <secs> <cmd...> — output lands in $TMP. Returns the command's status,
# or 124 if it timed out. No `timeout(1)` on macOS and coreutils is not assumed.
run_to() {
    _secs=$1; shift
    "$@" >"$TMP" 2>&1 &
    _cmd=$!
    ( _i=0
      while [ "$_i" -lt "$_secs" ]; do
          sleep 1
          kill -0 "$_cmd" 2>/dev/null || exit 0
          _i=$((_i + 1))
      done
      kill -9 "$_cmd" 2>/dev/null ) &
    _watch=$!
    wait "$_cmd" 2>/dev/null; _rc=$?
    kill "$_watch" 2>/dev/null
    wait "$_watch" 2>/dev/null
    # SIGKILL from the watchdog surfaces as 137; anything >=128 here is the
    # timeout firing, since these tools exit with small codes of their own.
    [ "$_rc" -ge 128 ] && return 124
    return "$_rc"
}

# A wedged libmtp session cannot be recovered in software — the USB-interface
# reset libmtp attempts on its own does not clear it. Say so plainly rather
# than letting the caller retry into the same wall.
check_wedged() {
    if grep -qE 'failed to open session|LIBMTP PANIC|Unable to open raw device' "$TMP" 2>/dev/null; then
        echo "error: the watch's MTP session is wedged (libmtp cannot reopen it)." >&2
        echo "       unplug the watch, replug it, and retry — no software reset clears this." >&2
        return 0
    fi
    return 1
}

# ---- 1. build present? ------------------------------------------------------
[ -f "$PRG" ] || {
    echo "error: $PRG missing — build it first (watch/README.md)" >&2
    exit 1
}

# ---- 2. staleness, BEFORE the transfer --------------------------------------
# This used to run after the send, where "rebuild before testing" arrives too
# late to stop you pushing a stale build and then debugging it on the watch.
NEWER=$(find "$PRJ/source" "$PRJ/resources" -type f -newer "$PRG" 2>/dev/null | head -1)
if [ -n "$NEWER" ]; then
    if [ "$SIDELOAD_FORCE" = "1" ]; then
        echo "warning: $NEWER is newer than the .prg — sending it anyway (SIDELOAD_FORCE=1)"
    else
        echo "error: $NEWER is newer than $PRG" >&2
        echo "       rebuild first (watch/README.md), or re-run with SIDELOAD_FORCE=1" >&2
        exit 1
    fi
fi

# ---- 3. is the watch even plugged in? ---------------------------------------
# Measured 2026-08-02: with no watch attached, mtp-filetree does not report "no
# raw devices" — it blocks, so you pay the full lookup timeout to find out. This
# check reads the USB tree via ioreg, which opens no MTP session and returns
# instantly. Garmin's vendor id is 0x091e (2334 decimal).
#
# Advisory only: if ioreg is missing (non-macOS), fall through and let the
# timeout be the backstop rather than refusing to run.
GARMIN_PID=""
if command -v ioreg >/dev/null 2>&1; then
    if ! ioreg -p IOUSB -w0 -l 2>/dev/null | grep -q '"idVendor" = 2334'; then
        echo "error: no Garmin device on USB (vendor 0x091e not present)" >&2
        echo "       plug the watch in — and use a data cable, not charge-only" >&2
        exit 1
    fi
    # Vendor alone is not enough: the watch also enumerates under other product
    # ids in non-MTP modes, where libmtp correctly reports no raw devices and
    # the bare "watch not found" message sends you hunting for a cable fault
    # that isn't there. Keep the pid so the diagnosis below can name it.
    GARMIN_PID=$(ioreg -p IOUSB -w0 -l 2>/dev/null | awk '/"idProduct"/ { print $NF; exit }')
fi

# ---- 4. resolve the Apps folder id ------------------------------------------
# libmtp's name paths fail on Garmin ("Parent folder could not be found") and
# mtp-sendfile has no -f flag, so a bare numeric id is the only reliable form.
# The id is not stable across devices — always look it up.
#
# mtp-detect used to run first purely to produce a friendlier "not connected"
# message. It is a full capability dump and one more session to wedge, and
# mtp-filetree already tells us everything, so it is gone.
echo "looking up GARMIN/Apps folder id..."
run_to "$LOOKUP_TIMEOUT" mtp-filetree
rc=$?
if [ "$rc" -eq 124 ]; then
    echo "error: mtp-filetree timed out after ${LOOKUP_TIMEOUT}s — watch connected and awake?" >&2
    exit 1
fi
check_wedged && exit 1
if grep -qiE 'No raw devices found|Unable to open' "$TMP" 2>/dev/null; then
    if [ -n "$GARMIN_PID" ]; then
        # Observed 2026-08-02: the fenix 8 sat on the bus as vendor 0x091e
        # product 0x0003 with no USB Product Name, while libmtp reported "No
        # raw devices found". Nothing is wrong with the cable or the port — the
        # watch simply is not presenting its MTP interface (0x51b5 on this
        # fenix 8). Waking/unlocking it and replugging brings MTP back.
        printf 'error: a Garmin is on USB (vendor 0x091e, product %s) but is not exposing MTP\n' "$GARMIN_PID" >&2
        echo "       libmtp reports no raw devices, so there is nothing to send to." >&2
        echo "       wake and unlock the watch, then unplug and replug it." >&2
        echo "       when MTP is up the fenix 8 enumerates as product 20917 (0x51b5)." >&2
    else
        echo "error: watch not found over USB (MTP mode? cable a data cable?)" >&2
    fi
    exit 1
fi

APPS=$(awk '$1 ~ /^[0-9]+$/ && $2 == "Apps" { print $1; exit }' "$TMP")
[ -n "$APPS" ] || {
    echo "error: no GARMIN/Apps folder on the watch" >&2
    exit 1
}

# ---- 5. duplicate scan — opt-in only ----------------------------------------
# `mtp-files` walks every object on the device and is the step that hung for
# ~4 minutes on 2026-08-02, before any transfer had started. All it buys is a
# warning: Garmin's MTP delete is unreliable (PTP 2002) and its enumeration is
# stale (mtp-files reports ids mtp-filetree no longer shows), so nothing is
# auto-removed either way. Duplicates are dev-noise, not corruption — the watch
# installs the newest copy since the app id matches. Off by default.
if [ "$SIDELOAD_CHECK_DUPES" = "1" ]; then
    echo "scanning for an existing app.prg (SIDELOAD_CHECK_DUPES=1; this can be slow)..."
    run_to "$LOOKUP_TIMEOUT" mtp-files
    if [ $? -eq 124 ]; then
        echo "note: mtp-files timed out after ${LOOKUP_TIMEOUT}s — skipping the duplicate check"
    else
        OLD=$(awk -v apps="$APPS" '
            /File ID: / { id = $3; want = 0 }
            /Filename: app\.prg/ { want = 1 }
            want && /Parent ID: / && $3 == apps { print id; exit }' "$TMP")
        [ -z "$OLD" ] || echo "note: GARMIN/Apps already has an app.prg (id $OLD) — this run adds another copy; clean up with: mtp-delfile -n $OLD"
    fi
fi

# ---- 6. send ----------------------------------------------------------------
# Output is captured to a file, not piped to `grep -q`: grep exits at the first
# match and closes the pipe, which can SIGPIPE mtp-sendfile mid-write. It also
# means a failure keeps libmtp's diagnostics instead of routing them to
# /dev/null, which previously made a failed send indistinguishable from a
# wedged one.
echo "sideload $PRG -> GARMIN/Apps (folder id $APPS)"
run_to "$SEND_TIMEOUT" mtp-sendfile "$PRG" "$APPS"
rc=$?
if [ "$rc" -eq 124 ]; then
    echo "error: mtp-sendfile timed out after ${SEND_TIMEOUT}s" >&2
    echo "       the watch may hold a partial file; replug and retry" >&2
    exit 1
fi
if ! grep -q 'New file ID' "$TMP"; then
    echo "error: mtp-sendfile failed" >&2
    sed 's/^/  | /' "$TMP" >&2
    exit 1
fi

FILE_ID=$(awk '/New file ID/ { print $NF; exit }' "$TMP")
echo "ok — transferred as file id ${FILE_ID:-?}"
echo "     unplug the watch; it installs from GARMIN/Apps on eject/boot"
