# Hardcoded timings

Every fixed delay, sleep, poll rate and timeout across the stack that a switch
or a coil can wait behind, collected so they can be tuned on purpose instead of
rediscovered. Written while getting Dirty Harry's diverters to react in time —
probably the most time-critical mech PPUC has to drive, because the ROM decides
on an opto while the ball is still travelling towards the diverter.

Line numbers drift; the names are what to search for.

## How a ROM's answer to a switch travels

A diverter on Dirty Harry goes through every stage below, in order. The bus
stages are paid twice: once to report the opto, once to send the coil.

| Stage | Where | Bounded by |
|---|---|---|
| Board debounce | io-boards | `debounce` + `debounceMode` per switch. Fast Flip reports a close at once; since firmware 0.4.3 a release only once the input has stayed open for the debounce window. |
| Wait to be polled | libppuc | one switch-chain pass, plus the idle interval when nothing is queued |
| Host → PinMAME | ppuc | main loop (20 µs) and the time fence (1 ms steps) |
| ROM decides | ROM | the ROM's own switch scan; about 5 ms for a WPC ROM to accept a closure |
| PinMAME → host | ppuc | coil poll thread, 1 kHz |
| Wait for the next output frame | libppuc | the rest of the switch chain in progress; since libppuc c615c4b the idle interval no longer adds to it |
| Coil pulls in | mechanics | power and the mech's own travel |

A board marked `firstInChain` reports early in the pass, so its answer usually
leaves with the next output frame instead of the one after (libppuc 094e349).

## Runtime — on the latency path every pass

These are the ones worth tuning.

| Constant | Value | Where | What it costs | Notes |
|---|---|---|---|---|
| `kBaudRate` | 250000 | `io-boards/src/PPUCProtocolV2.h` | wire time of every frame and reply | The ADM3483's rated maximum since firmware 0.4.0 (was 115200). No headroom left above it. |
| Output frame interval (`OutputFrameIntervalMs`, `RS485_COMM_DEFAULT_OUTPUT_FRAME_INTERVAL_MS`, `kDefaultOutputFrameIntervalMs`) | 4 ms | `libppuc/src/RS485Comm.h`, `ppuc/src/ppuc.cpp`, config-tool `field_ini_output_frame_ms` | Idle wait before the next pass when nothing is queued. A busy game queues lamp snapshots and skips it; a quiet stretch pays it on **every** pass, which delays every switch report by up to that much. | **Candidate.** A coil change now cuts the wait short, but a switch still waits for the next pass. Try 2 or 1 ms on Dirty Harry; the cost is bus occupancy, not correctness. Settable per game in config-tool. |
| `RS485_MODE_SWITCH_DELAY` | 50 µs, twice per reply | `io-boards/src/EventDispatcher/Event.h` | 100 µs per board per pass: 0.5 ms a pass on a five-board chain | **Candidate, after measuring.** The ADM3483's driver enable is in the hundreds of nanoseconds; this covers the host adapter's turnaround and loop jitter. Lower only with the DE lines on a scope, see `BUS_MEASUREMENT.md`. Also referenced by `kReadTimeoutSlackUs`. |
| `switchReplyDelayUs` (`SwitchReplyDelayUs`) | 0 | firmware, libppuc, ppuc, config-tool | Since firmware 0.4.4 once per pass, before the reply that follows the host's frame (before: delay × boards). Plus a quarter of it as post-TX settle after every reply. | Depends on the host adapter: 300–500 µs on a Raspberry Pi carrier board, whose UART releases the bus in software, 0 on a USB adapter with automatic direction control. See `RS485_HOST_ADAPTERS.md`. |
| `RS485_COMM_SLOW_SWITCH_POLL_DIVIDER` | 8 | `libppuc/src/RS485Comm.h` | a `slowSwitches` board is polled every eighth pass | Not used on Dirty Harry: the gun trigger on the cabinet board needs every pass. |
| Coil poll (`coilPollHz`) | 1000 Hz | `ppuc/src/PluginEngine.h` | up to 1 ms between the ROM setting a coil and the host seeing it | Could go to 2000 Hz; worth little against the bus. |
| `timeFenceLeadUs` | 1000 µs | `ppuc/src/PluginEngine.h` | how far PinMAME may run ahead of the wall clock | `--no-time-fence` restores the old 16 ms bursts, for comparison only. Not exposed as a value. |
| `MAIN_LOOP_SLEEP_US` | 20 µs | `ppuc/src/ppuc.cpp` | between main-loop passes, where switches are drained to the engine | Fine. |
| `kDefaultSwitchMinHoldMs` (`SwitchMinHoldMs`) | 10 ms | `ppuc/src/ppuc.cpp`, config-tool | Delays only a reversal that comes back sooner; never the first edge. | Measured on Dirty Harry; see the commit introducing it. |
| `m_coilHoldFrameCount` (`--coil-hold-frames`) | 3 frames | `libppuc/src/RS485Comm.h` | none: keeps a short coil pulse in the next three output frames so it cannot fall between two | Not on the latency path. |
| `outputPollHz` | 120 Hz | `ppuc/src/PluginEngine.h` | lamps and GI sampled from PinMAME | Invisible on a lamp. |
| `MATRIX_SWITCH_DEBOUNCE` | 2 ms | `io-boards/src/IODevices/SwitchMatrix.h` | lockout per edge on matrix switches | Keeps both edges. |
| `STROBED_MATRIX_DEBOUNCE_US` | 2000 µs | `io-boards/src/IODevices/StrobedSwitchMatrixLogic.h` | as above, strobed matrix | |
| `SWITCH_DEBOUNCE` | 10 ms | `io-boards/src/IOBoardController.cpp` | default debounce of a dedicated switch | Overridden per switch by the YAML. In `standard` mode a closure shorter than the window is **dropped**; use Fast Flip on anything a ball touches briefly. |
| `STOP_SWITCHED_MAX_PULSE_MS` | 40 ms | `io-boards/src/IODevices/PwmDevices.h` | applied to a coil with a stop switch and no max pulse | Safety net for flippers. |

## Robustness — only paid when something goes wrong

Generous on purpose. Shortening them makes recovery faster and false alarms more
likely; leave them unless a measurement says otherwise.

| Constant | Value | Where | Purpose |
|---|---|---|---|
| `SwitchReplyWindowUs()` base | 40 ms (+ reply delay × boards) | `libppuc/src/RS485Comm.cpp` | Total window for a whole switch chain before a miss is counted. ~3× a five-board chain. |
| `RS485_COMM_SERIAL_READ_TIMEOUT` | 5 ms | `libppuc/src/RS485Comm.h` | Floor of a single blocking read inside the window. |
| `RS485_COMM_SERIAL_WRITE_TIMEOUT` | 20 ms | `libppuc/src/RS485Comm.h` | Blocking write of one frame. |
| `RS485_COMM_SWITCH_REPLY_MISS_THRESHOLD` | 3 | `libppuc/src/RS485Comm.h` | Consecutive missed chains before a session resync. |
| `FrameWireTimeUs()` guard | wire time + 25 % + 200 µs | `io-boards/src/EventDispatcher/EventDispatcher.cpp` | Board-side read deadline; derived from `kBaudRate`, scales with it. |
| `kReadTimeoutSlackUs` | 500 µs | same | Extra allowance before a partial frame is abandoned. |
| Watchdog: loop stalled | 1000 ms | `io-boards/src/main.cpp` | High-power outputs off. |
| Watchdog: no poll | 3000 ms | same | High-power outputs off — what protects a coil with no max pulse time if the host dies. |
| `WATCHDOG_REBOOT_MS` | 5000 ms | same | Board reboot. |
| `LOOP_STALL_REPORT_US` | 20 ms | `ppuc/src/ppuc.cpp` | Logs a main-loop pass that took longer. |

## Startup, configuration and administration — never at runtime

Listed so nobody hunts them for a runtime problem.

| Constant | Value | Where |
|---|---|---|
| Config frame pacing | 5 ms per frame | `libppuc/src/RS485Comm.cpp` (`SendConfigEvent`) |
| Mapping frame pacing | 5 ms per frame | `libppuc/src/RS485Comm.cpp` (`SendMappingFrame`) |
| `RS485_COMM_CONFIG_ACK_TIMEOUT_US` × `RS485_COMM_CONFIG_ACK_RETRIES` | 50 ms × 3 | `libppuc/src/RS485Comm.h` |
| `RS485_COMM_SWITCH_POLL_STARTUP_HOLD_MS` | 250 ms | `libppuc/src/RS485Comm.h` |
| `RS485_COMM_EFFECT_EVENT_SPACING_US` | 1 ms per effect event | `libppuc/src/RS485Comm.h` |
| `RS485_COMM_ADMIN_FRAME_ASSEMBLY_MS`, `RS485_COMM_ADMIN_SETTLE_MS` | 50 ms, 60 ms | `libppuc/src/RS485Comm.h` |
| `RS485_COMM_UPDATE_BEGIN_TIMEOUT_MS` | 30 s | `libppuc/src/RS485Comm.h` |
| `kAdminReplyDelayUs` | 2000 µs | `io-boards/src/EventDispatcher/EventDispatcher.cpp` |
| `kBoardBootWaitMs` | 2000 ms | `ppuc/src/ppuc.cpp` |
| Connect, restart and disconnect sleeps | 10–400 ms | `libppuc/src/RS485Comm.cpp` (`Connect`, `RestartBoards`, `Disconnect`) |
| `WAIT_FOR_IO_BOARD_RESET` | 5 s | `io-boards/src/PPUCTimings.h` |
| Reboot and board-id sampling delays | 1–10 ms | `io-boards/src/IOBoardController.cpp`, `EffectsController.h` |

## Not in the code, but on the path

- **The host's serial adapter.** On the Pi the bus is the PL011 UART
  (`/dev/ttyAMA0`, kernel RS485 mode, no added latency). A USB-RS485 adapter on
  a Mac or PC adds its own: FTDI chips default to a 16 ms latency timer, which
  would dwarf everything above. Bench numbers from a USB adapter do not carry
  over to the cabinet.
- **The mech.** A diverter's plunger needs roughly 20–30 ms to travel, which no
  timing above can shorten. Full pull-in power is what can.

## Order to try

1. Measure the pass on the machine — pass length and the gap between board
   replies. That says how much is wire time and how much is fixed per board.
2. `OutputFrameIntervalMs` down to 2, then 1, watching `PPUCBusHealth` for
   missed chains.
3. `RS485_MODE_SWITCH_DELAY` down, only with the DE lines on a scope.
4. Only then the split chain (`CPU 5 1 2 CPU 3 4`), which can be done host-side
   by configuring two chains and entering them alternately, at the cost of each
   board being polled every other pass.
