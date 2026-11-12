# RS485 host adapters and the reply turnaround

How the host gets onto the PPUC bus, what differs between the ways of doing it,
and what `SwitchReplyDelayUs` is for. Written after Dirty Harry ran clean from a
MacBook with a USB adapter and dropped flippers on a Raspberry Pi carrier board,
with the same boards, firmware and game.

The short version:

| Host adapter | Who switches the transceiver's direction | `SwitchReplyDelayUs` |
|---|---|---|
| Raspberry Pi UART on a carrier board (`/dev/ttyAMA0`) | the Linux kernel, in software | **300–500** |
| USB-RS485 adapter with automatic direction control (Mac, Linux, Windows) | the adapter, in hardware | **0** |
| Any other UART or adapter without automatic direction control | nobody | not supported |

## The bus, briefly

RS485 here is one differential pair shared by the host and every board, half
duplex. Exactly one device may drive it at a time; everyone else listens. Each
device has a transceiver with a **driver enable** (DE) line: raised to talk,
dropped to listen.

The host sends an output frame whose header names the first board to answer.
That board replies and names the next, and so on down the chain
([`V2_PROTOCOL.md`](V2_PROTOCOL.md) §1, §9). So within one poll round there are
two kinds of handover:

1. **host → first board.** The host has just finished its frame and must drop
   its DE before the first board starts driving.
2. **board → board.** A board has just finished its reply and must drop its DE
   before the next one starts.

The second kind is fast and always was: a board releases DE in firmware the
moment the RP2040's UART reports the last bit has left the shift register
(`ReleaseBusAfterTx`). The first kind depends entirely on the host's adapter,
and that is where the adapters differ.

## Raspberry Pi carrier board

The Pi talks to the bus through its PL011 UART on GPIO 14/15, `/dev/ttyAMA0`
(Bluetooth disabled with `dtoverlay=disable-bt`). The carrier's transceiver is
the same ADM3483 the boards carry. Its DE is driven by the UART's RTS line,
which zedmdos routes to GPIO 17 with `gpio=16-17=a3` in `config.txt`.

libppuc puts that port into the kernel's RS485 mode (`TryEnableHardwareRs485()`
in `libppuc/src/RS485Comm.cpp`, `TIOCSRS485` with `SER_RS485_RTS_ON_SEND`). It
does this only for `/dev/ttyAMA0` and `/dev/serial0`, or for any port when
`PPUC_RS485_HW=1` is set. With `--debug` it prints `RS485 HW mode enabled on
/dev/ttyAMA0 (RTS as DE)`.

"Hardware mode" is the kernel's name for it, but on this UART the release is
**software**. The kernel raises RTS before sending. It drops RTS only once its
transmit-complete interrupt has run and it has seen the shift register empty.
How long after the last bit that is depends on how quickly the interrupt is
served. That is microseconds on an idle Pi, and noticeably more when other
interrupt sources compete. USB isochronous audio is one example; a USB sound
card configured as output made the boards unreachable outright.

Meanwhile the first board in the chain answers the moment the host's frame
ends. If the Pi's DE is still up, both drive the pair at once and the frames
collide.

### What a late release looks like

There is no CRC error on the host side, because the collision usually costs the
first board's reply or the frame it was answering, not a bit in the middle of
something the host reads. The log shows it as:

- the **first board in the chain** and only that board reporting
  `flags=0x19`, `0x1C` or `0x09` — sequence gap (0x08: it missed an output
  frame) and parser resynced (0x10: it saw a damaged one);
- `Timed out waiting for V2 switch reply for board token N … sawBytes=no`
  for that same board;
- `Starting V2 session resync` every minute or so, and the `bus health` line at
  exit showing missed chains and resyncs.

Every resync restarts the session with all boards, and the boards report their
switches afresh. In a game that shows up as all three flipper buttons reading
released at once and the ROM dropping its hold windings. That is the "flipper
drops while held" symptom, even though the flippers are fired board-locally.

On Dirty Harry the first board is the opto board in front of the diverters,
put there with `firstInChain: true` so that the ROM's answer catches the next
output frame. Before that it answered last, after another board. The problem
appeared in the same days as that change, the move to 250000 baud, which
shortens every gap on the wire, and the move from the Mac to the Pi.

### Measured on Dirty Harry

Firmware 0.4.2, five boards, one game each, `bus health` at exit:

| `SwitchReplyDelayUs` | missed chains | resyncs | flippers |
|---|---|---|---|
| 0 | 24 | 8 | dropped while held |
| 300 | 7 | 1 | fine |

### Recommendation

Set `SwitchReplyDelayUs` to **300–500** for a Pi carrier board, in the
config-tool ("Runtime: SwitchReplyDelayUs") or in `ppuc.ini` under `[Runtime]`.
If the first board still reports sequence gaps or resyncs, raise it; on
firmware 0.4.4 and later it costs one delay per poll round, not one per board.

Keep the Pi's interrupt load in mind: anything that keeps CPU 0 busy with
interrupts delays the release further. Leave USB audio and ZeDMD over SPI off
unless they are actually needed.

## USB-RS485 adapters

A USB adapter is opened like any serial port (libserialport, RTS forced off,
no flow control). libppuc does **not** toggle RTS for it, so the adapter has to
switch its own transceiver. Adapters built for half-duplex RS485 do that in
hardware: the driver is enabled while the adapter's UART is transmitting and
released when it stops, with no host software in the loop. FTDI chips do it with
their TXDEN output, and many small CH340 and CP210x modules with an
auto-direction circuit on the board.

That release is immediate, so the first board can answer straight away:
`SwitchReplyDelayUs = 0` is right for these adapters. This is why the same
machine ran clean from a MacBook.

What USB adds instead is **latency per transfer**, typically around a
millisecond and up to the adapter's latency timer. That slows the poll rate,
not the turnaround, and it is the main reason a USB host polls the boards less
often than a Pi on its own UART.

An adapter **without** automatic direction control does not work: nothing
raises its DE, so the host never reaches the bus. A loose wire inside the
adapter looks the same, so rule that out before blaming the firmware.

## What `SwitchReplyDelayUs` does

Before firmware 0.4.4, every board waited `SwitchReplyDelayUs` before raising
DE, whoever spoke before it. A five-board chain paid five times the delay per
poll round, though only the first handover needed it.

Since 0.4.4 a board waits only when the token reached it **in the host's own
frame**: an output frame or a switch refresh frame
(`EventDispatcher::forwardSwitchTokenIfSelected(…, afterHostFrame)`). That is
the first board of the round. Usually that is the board marked
`firstInChain`. When libppuc starts a round further down the chain, for example
to skip boards that carry only slow switches, it is whichever board that round
starts at. Every board that receives the token from another board answers at
once.

The post-TX settle (`SwitchReplyDelayUs / 4`, capped at 2 ms) still applies
after every board's reply. It stalls the board after it has already released
the bus and protects nothing on the wire; see `V2_PROTOCOL.md` §12.

On the host side two things still use the delay:

- `SwitchReplyWindowUs()` budgets 40 ms plus the delay for every board. That is
  generous now, and only affects how long the host waits before counting a
  chain as missed.
- For virtualized boards, which the host answers itself, libppuc still sleeps
  the delay before every virtual reply.

Admin replies (version reports, update acknowledgements) have their own fixed
2 ms turnaround, `kAdminReplyDelayUs`, and do not use this setting.

## Checking a setup

1. Play a game with switches worked hard; an attract-mode run does not exercise
   the chain enough.
2. Stop ppuc. With Serum loaded, `kill -TERM` takes the emergency exit, which
   prints the report since ppuc `56a447d`.
3. Read the counters:

   ```bash
   grep -A15 "bus health" /userdata/dumps/log.txt | tail -18
   ```

   `missed` and `session resync(s)` should be at or near zero, and no board
   should appear repeatedly with `flags=0x19`, `0x1C` or `0x09`. If one does
   and it is the first in the chain, raise `SwitchReplyDelayUs`. If it is a
   board further down, the cause is elsewhere: its own receive path,
   termination, or wiring.

## Related settings

- `firstInChain` in the game YAML decides which board answers first, and so
  which board needs the delay ([`V2_PROTOCOL.md`](V2_PROTOCOL.md), "A board
  that answers first").
- `OutputFrameIntervalMs` sets how often output frames, and so poll rounds,
  go out when nothing else is queued ([`TIMINGS.md`](TIMINGS.md)).
- `SwitchIdleRefreshPolls` (firmware 0.4.2 and later) sets after how many quiet
  polls a board re-reads its switches by itself.
