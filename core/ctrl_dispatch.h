#pragma once

#include <stdint.h>

/* Callback used by ctrl_dispatch() to send a response line.
 * The implementation must copy `msg` if it needs it after returning. */
typedef void (*ctrl_tx_fn)(const char *msg, void *ctx);

/* Bytes of console output lost WHILE A CONSOLE WAS ATTACHED, cumulative since
 * boot, reported by STATUS as "tx_drops". Output emitted with no console
 * attached is drained by design and deliberately not counted (on USB-CDC the
 * stack refuses every write while DTR is clear, so "before you attached" is
 * always lost in full — counting it would drown the signal). Deliberately not
 * reset when the port re-opens: an earlier version did, which zeroed the
 * history at the only moment it became readable.
 *
 * A console that silently loses output is worse than one that is merely slow:
 * a missing log line reads as "that code path did not run". This makes the loss
 * visible instead. Non-zero means the FIFO overflowed or the USB stack refused
 * a write during some attached session, so treat surrounding logs as
 * incomplete.
 *
 * Weakly defined in core as 0. The platform overrides it — on nRF5 that is
 * firmware/usb_cdc_log.c. Declared here rather than in machine.h on purpose:
 * machine.h is the treadmill facade, and this is a property of the console
 * transport, which core otherwise knows nothing about. */
uint32_t ctrl_console_drops(void);

/* Which image is running, reported by STATUS as "build" — e.g.
 * "0811-1712 eed970d+" (MMDD-HHMM local build time, git revision, '+' if the
 * tree had uncommitted changes).
 *
 * Exists because a flash that silently did not take is indistinguishable from
 * one that worked: the board comes up and answers STATUS either way, and there
 * was previously nothing to read back that names the image. Debugging code that
 * was never on the device has already cost this project a session on the watch
 * side, which is why the data field renders the same kind of stamp.
 *
 * The returned string is emitted into a JSON value UNESCAPED, so an
 * implementation must return plain ASCII containing no '"' and no '\'. It must
 * also have static storage duration — the caller does not copy it.
 *
 * Weakly defined in core as "unknown" (host tests link core alone and have no
 * image to name). The platform overrides it — on nRF5 that is the generated
 * firmware/build_stamp.c, written by tools/fw_stamp.sh. Same seam as
 * ctrl_console_drops() above, and for the same reason: core cannot know it. */
const char *ctrl_build_stamp(void);

/* ANT+ common page 70 request traffic on the footpod channel, reported by
 * STATUS as "p70" (requests accepted), "p70_rej" (page 70s we could not act
 * on), and "p70_pg" (the page the last accepted request asked for).
 *
 * This is an experiment readout, not telemetry. docs/sdm-recording-analysis.md
 * §12 leaves exactly two explanations for the watch recording a zero cadence
 * from us: it learns capabilities only by *asking* with page 70, or it ignores
 * capabilities entirely. Those look identical in a .FIT. They do not look
 * identical here — re-pair the footpod with the console attached and read the
 * counter. Zero through a re-pair and a run start means the watch never asks,
 * and no amount of capabilities work can fix cadence.
 *
 * Weakly defined in core as 0, like the two above: core has no radio. The
 * platform overrides them — on nRF5 that is firmware/ant_sdm.c, which owns the
 * ant_request_t these read. */
uint32_t ctrl_sdm_p70_requests(void);
uint32_t ctrl_sdm_p70_rejects(void);
uint8_t  ctrl_sdm_p70_last_page(void);

/* ANT+ sniffer channel — the console side of firmware/ant_log.c, driven by the
 * `ANTLOG` command. It opens a second ANT slave channel that locks onto some
 * OTHER sensor in the room and dumps its pages to the console, so a real
 * footpod (or a treadmill's own ANT+ broadcast) and the one this bridge
 * synthesizes can be logged from one radio against one clock.
 *
 * Wildcards: 0 for dev_num / dev_type / trans_type each mean "any". period 0
 * means "pick from dev_type" and freq 0 means 57 (ANT+). start() returns
 * NRF_SUCCESS (0) or a SoftDevice error code; the console prints the number
 * rather than swallowing it, because every plausible failure here (channel not
 * allocated, wrong state, bad period) is distinguished only by that code.
 *
 * Weakly defined in core as "no radio", like the p70 counters above — core has
 * no radio and the host tests link core alone. Firmware overrides them from
 * firmware/ant_log.c. */
uint32_t ctrl_antlog_start(unsigned dev_num, unsigned dev_type,
                           unsigned trans_type, unsigned period, unsigned freq);
uint32_t ctrl_antlog_stop(void);
void     ctrl_antlog_status(char *buf, unsigned cap);

/* Parse and execute one ASCII command line.
 * `tx` is called zero or more times with JSON response strings (no trailing \n).
 * Thread-safe: individual commands are atomic against concurrent dispatches
 * because the underlying machine_* calls are already serialised. */
void ctrl_dispatch(const char *line, ctrl_tx_fn tx, void *ctx);
