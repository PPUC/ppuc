# PPUC RS485 Protocol, Version 2

Reference for the wire protocol between the host (`libppuc`, driven by `ppuc`)
and the RP2040 IO boards.

**`io-boards/src/PPUCProtocolV2.h` is the authoritative definition.** This
document describes what that header specifies and why; where the two disagree,
the header is right and this file is stale. Every constant below is taken from
the header and can be checked against it.

The format is pinned by a conformance suite in
`io-boards/test/conformance/ProtocolConformance.cpp`, which is compiled into
**both** the firmware tests and the host tests, so both sides assert the same
contract from identical code. If you change anything described here, that suite
is what tells you.

---

## Contents

1. [Transport](#1-transport)
2. [Frame structure](#2-frame-structure)
3. [Encoding rules](#3-encoding-rules)
4. [Frame types](#4-frame-types)
5. [Flags, status and sizing](#5-flags-status-and-sizing)
6. [Payload layouts](#6-payload-layouts)
7. [Admin channel and firmware update](#7-admin-channel-and-firmware-update)
8. [Session lifecycle](#8-session-lifecycle)
9. [Runtime loop](#9-runtime-loop)
10. [Configuration topics](#10-configuration-topics)
11. [Timing](#11-timing)
12. [Missing features and future work](#12-missing-features-and-future-work)

---

## 1. Transport

| Property | Value |
|---|---|
| Physical layer | RS485, half duplex, one differential pair |
| Transceiver | ADM3483 (slew-rate limited, 250 kbps ceiling) |
| Baud rate | **115200** (`kBaudRate`) |
| Framing | 8N1 |
| Max boards | **8** (`kMaxBoards`) |
| Board addressing | 0…7, with `0xFF` (`kNoBoard`) meaning "no specific board" |

One pair is shared by the host and every board, so exactly one device may drive
the line at a time. Each board controls its transceiver with a **DE** (driver
enable) GPIO and returns to receive mode as soon as its reply has left the wire.

Boards are not polled individually by address. The host names the next board to
speak in the frame header (`nextBoard`), and each board's reply names the one
after it — a **token chain**. The host walks the chain in
`RS485Comm::ReceiveSwitchStateChain()` and knows the expected order from
`GetLogicalNextSwitchBoard()`.

### Pin assignment on the board

| Signal | RP2040 GPIO |
|---|---|
| UART TX | 0 (`RS485_TX_PIN`) |
| UART RX | 1 (`RS485_RX_PIN`) |
| Driver enable (DE) | 2 (`RS485_MODE_PIN`) |

GPIO 0/1 are UART0 on the RP2040, and the firmware depends on that: it releases
DE by polling `uart0`'s BUSY flag directly. `main.cpp` carries a `static_assert`
so moving the UART fails the build rather than truncating frames.

---

## 2. Frame structure

Every frame is a 5-byte header, an optional payload, and a 2-byte CRC:

```
 byte  0     1              2          3         4        5 …        n-1  n
      +-----+--------------+----------+---------+--------+----------+--------+
      |sync |typeAndFlags  |nextBoard |sequence |epoch   | payload  |  CRC   |
      |0xA5 |              |          |         |        | (0…n)    | 16 bit |
      +-----+--------------+----------+---------+--------+----------+--------+
      \___________________ covered by the CRC ____________________/
```

| Field | Size | Meaning |
|---|---|---|
| `sync` | 1 | Always `0xA5` (`kSyncByte`). Frame start and resync anchor. |
| `typeAndFlags` | 1 | Low nibble = frame type, high nibble = flags. |
| `nextBoard` | 1 | Board that may transmit after this frame, or `0xFF`. |
| `sequence` | 1 | Sender's frame counter, wraps at 256. |
| `epoch` | 1 | Session generation. See [§8](#8-session-lifecycle). |
| `payload` | 0…n | Type-dependent, see [§6](#6-payload-layouts). |
| `crc` | 2 | CRC-16/CCITT-FALSE over header **and** payload, big-endian. |

`kHeaderBytes = 5`, `kCrcBytes = 2`.

### CRC

CRC-16/CCITT-FALSE: initial value `0xFFFF`, polynomial `0x1021`, no input or
output reflection, no final XOR. Standard check value for `"123456789"` is
`0x29B1`, and the conformance suite asserts exactly that.

Computed over the header and payload, appended big-endian. A receiver that
fails the check discards the frame and hunts for the next `0xA5`.

---

## 3. Encoding rules

**All multi-byte fields are big-endian.**

**Frames are assembled one byte at a time and never by copying a struct.** This
is deliberate and load-bearing: the host runs on x86-64, aarch64 and 32-bit
Windows, the boards on an RP2040, and struct layout, padding and endianness
differ across those. Serializing a struct would put the compiler's ABI on the
wire.

The `struct`s in the header exist to *describe* the layout to a reader. They are
not serialized. Because the frame-size constants are nevertheless derived from
`sizeof()` on them, the header carries `static_assert`s pinning each to its wire
size, so a padding compiler fails the build instead of silently emitting
wrong-length frames:

```
FrameHeader          == 5    ConfigPayload    == 8
SetupPayload         == 6    ConfigAckPayload == 8
MappingPayload       == 6    TriggerPayload   == 4
switch status prefix == 4    kGiBytes         == 3
```

The single definition of the byte order lives in the header's **wire codec**
section (`WriteU16`/`ReadU16`, `WriteU32`/`ReadU32`, `WriteHeader`/`ReadHeader`,
`AppendCrc`/`VerifyCrc`, and the `Build*Frame` helpers). Both sides use it, so
the byte order has one implementation rather than two that can drift apart.

---

## 4. Frame types

Type is the **low nibble** of `typeAndFlags` (`ExtractType`), flags the high
nibble (`ExtractFlags`). Values are on the wire; renumbering breaks every
deployed board.

| Value | Name | Direction | Payload | Frame size |
|---|---|---|---|---|
| `0x01` | `kFrameOutputState` | host → boards | coils + lamps + GI bitmaps | runtime, see [§5](#5-flags-status-and-sizing) |
| `0x02` | `kFrameSwitchState` | board → host | status + switch bitmap | runtime |
| `0x03` | `kFrameHeartbeat` | *(reserved)* | none | — |
| `0x04` | `kFrameError` | *(reserved)* | none | — |
| `0x05` | `kFrameSetup` | host → boards | `SetupPayload` | 13 |
| `0x06` | `kFrameMapping` | host → boards | `MappingPayload` | 13 |
| `0x07` | `kFrameReset` | host → boards | none | 7 |
| `0x08` | `kFrameConfig` | host → boards | `ConfigPayload` | 15 |
| `0x09` | `kFrameSwitchNoChange` | board → host | status only | 11 |
| `0x0A` | `kFrameConfigAck` | board → host | `ConfigAckPayload` | 15 |
| `0x0B` | `kFrameRestart` | host → boards | none | 7 |
| `0x0C` | `kFrameTrigger` | host → boards | `TriggerPayload` | 11 |
| `0x0D` | `kFrameSwitchRefresh` | host → boards | none | 7 |
| `0x0E` | `kFrameAdmin` | both | `AdminPayload`, or an update body | 21, or see [§7](#7-admin-channel-and-firmware-update) |

`0x0F` is the **only free type value left**. The type field is a nibble, so
there is no room to grow: anything further has to go inside an existing
envelope, which is what `kFrameAdmin` is for.

`kFrameHeartbeat` and `kFrameError` are **defined and parsed but never sent by
either side.** Both parsers accept and skip them. See
[§12](#12-missing-features-and-future-work).

### Reset versus Restart

Two recovery frames with quite different force:

- **`kFrameRestart` (0x0B)** is the normal path. It clears board-local
  configuration and runtime state and turns outputs off, while the RP2040 stays
  alive and on the UART. This is the standard startup and shutdown path.
- **`kFrameReset` (0x07)** is a hard reboot of the board, reserved for genuine
  faults. It depowers outputs, so it is not something to reach for on ordinary
  transport trouble.

---

## 5. Flags, status and sizing

### Frame flags (high nibble)

| Value | Name | Meaning |
|---|---|---|
| `0x00` | `kFlagNone` | — |
| `0x10` | `kFlagKeyframe` | Payload is a complete state, not a delta. |
| `0x20` | `kFlagDelta` | Payload is a change relative to the last keyframe. |
| `0x80` | `kFlagError` | Error indication. |

### Switch status flags

Reported by boards in the 4-byte status prefix of both switch reply variants:

| Value | Name | Meaning |
|---|---|---|
| `0x01` | `kStatusInSync` | Board believes it is in session with the host. |
| `0x02` | `kStatusNeedsSetup` | No valid `SetupFrame` seen this session. |
| `0x04` | `kStatusMappingIncomplete` | Device mapping not fully received. |
| `0x08` | `kStatusSequenceGap` | A gap was observed in the host's sequence. |
| `0x10` | `kStatusParserResynced` | The parser had to hunt for sync. |
| `0x20` | `kStatusSwitchOverflow` | Switch events were dropped. |

### Config ack status

| Value | Name |
|---|---|
| `0x00` | `kConfigAckAccepted` |
| `0x01` | `kConfigAckRejected` |

### Runtime sizing

Device counts are per game, announced by `SetupFrame`, and decide the size of
the two variable frames. Bitmaps are indexed by **global device number**: bit N
is device N.

| Constant | Default | Maximum |
|---|---|---|
| `coilBits` | 24 | 64 |
| `lampBits` | 64 | 256 |
| `switchBits` | 64 | 256 |

```
BitsToBytes(n)          = (n + 7) / 8
OutputPayloadBytes      = BitsToBytes(coilBits) + BitsToBytes(lampBits) + kGiBytes
SwitchPayloadBytes      = kSwitchStatusBytes + BitsToBytes(switchBits)
SwitchNoChangePayload   = kSwitchStatusBytes                      (= 4)
<any>FrameBytes         = kHeaderBytes + payload + kCrcBytes
```

`IsValidRuntimeConfig()` requires every count to be non-zero and within its
maximum.

### General illumination

5 strings (`kGiStrings`) at 4 bits each (`kGiLevelBits`), levels 0…8
(`kMaxGiLevel`), packed two per byte into **3 bytes** (`kGiBytes`). Levels above
the maximum are clamped by `ClampGiLevel()`; the packing helper is
`SetPackedNibble()`.

---

## 6. Payload layouts

All fields big-endian. Offsets are from the start of the payload, i.e. byte 5 of
the frame.

**`SetupPayload` — 6 bytes**

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `coilBits` |
| 2 | 2 | `lampBits` |
| 4 | 2 | `switchBits` |

**`MappingPayload` — 6 bytes**

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `domain` — `0x01` coil, `0x02` lamp, `0x03` switch |
| 1 | 1 | reserved |
| 2 | 2 | `index` |
| 4 | 2 | `number` |

**`ConfigPayload` — 8 bytes**

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `boardId` |
| 1 | 1 | `topic` — see [§10](#10-configuration-topics) |
| 2 | 1 | `index` |
| 3 | 1 | `key` |
| 4 | 4 | `value` |

**`ConfigAckPayload` — 8 bytes**

| Offset | Size | Field |
|---|---|---|
| 0…3 | 4 | `boardId`, `topic`, `index`, `key` — echoed from the request |
| 4 | 1 | `status` |
| 5 | 3 | reserved |

**`TriggerPayload` — 4 bytes**

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `source` |
| 1 | 2 | `number` (hi, lo) |
| 3 | 1 | `value` |

**`SwitchPayload` — 4 + `BitsToBytes(switchBits)`**

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `epochSeen` — the epoch this board last accepted |
| 1 | 1 | `lastHostSequenceSeen` |
| 2 | 1 | `statusFlags` |
| 3 | 1 | reserved |
| 4 | n | switch bitmap (absent on `kFrameSwitchNoChange`) |

**`AdminPayload` — 14 bytes**

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `command` — see [§7](#7-admin-channel-and-firmware-update) |
| 1 | 1 | `boardId` — the board this concerns |
| 2 | 12 | `data`, interpreted per command |

The update commands reuse only the first two bytes and then carry a body of
their own size, so an `AdminFrame` and an update frame are not the same length.

**`OutputPayload`**

Coil bitmap, then lamp bitmap, then 3 GI bytes. Only
`BitsToBytes(coilBits)` and `BitsToBytes(lampBits)` bytes are on the wire — the
struct's `kMaxCoilBytes`/`kMaxLampBytes` arrays are buffer capacity, not frame
size.

---

## 7. Admin channel and firmware update

`kFrameAdmin` (`0x0E`) is an envelope, not a single message. Its first two
payload bytes are always `command` and `boardId`; everything after that is the
command's own. This exists because the type nibble was nearly exhausted — see
[§4](#4-frame-types) — so anything added from here on shares one type value.

Admin frames are addressed to a single board by `boardId` in the payload, not
by the header's `nextBoard`, which stays `kNoBoard`. Boards that are not
addressed ignore the frame and pass it on. Addressing matters here more than
elsewhere: administration happens outside the switch chain, so no token decides
who may transmit, and a broadcast query would put every board on the wire at
once.

Admin handling is deliberately **not** gated on a valid runtime config or a
matching epoch. The point of a version query is to work before a session
exists — that is exactly when the host needs to know what it is talking to.

| Value | Command | Direction | Frame size |
|---|---|---|---|
| `0x01` | `kAdminVersionQuery` | host → board | 21 |
| `0x02` | `kAdminVersionReport` | board → host | 21 |
| `0x03` | `kAdminUpdateBegin` | host → board | 15 |
| `0x04` | `kAdminUpdateBeginAck` | board → host | 14 |
| `0x05` | `kAdminUpdateChunk` | host → board | 15 + chunk, max 271 |
| `0x06` | `kAdminUpdateChunkAck` | board → host | 14 |
| `0x07` | `kAdminUpdateCommit` | host → board | 9 |
| `0x08` | `kAdminUpdateResult` | board → host | 14 |

The chunk frame at 271 bytes is by a wide margin the largest frame on this bus;
the next largest is an output frame at maximum device counts, 50 bytes.
`kMaxFrameBytes` is 271 for this reason, and any receive buffer that is not is
an overrun waiting for the first update.

### Version report

The 12-byte data area of `kAdminVersionReport`:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | firmware major |
| 1 | 1 | firmware minor |
| 2 | 1 | firmware patch |
| 3 | 1 | admin protocol major |
| 4 | 1 | admin protocol minor |
| 5 | 1 | capabilities — `0x01` can report, `0x02` can self-update |
| 6 | 1 | board type — see below |
| 7 | 1 | reserved |
| 8 | 4 | build id |

The **board type** is on the wire because firmware is board-specific. The same
GPIO is a coil output on one board and an input on another, so an image flashed
to the wrong type does not merely misbehave, it drives an output into an input.
The host matches type before it sends a byte.

| Value | Name |
|---|---|
| `0x00` | unknown |
| `0x01` | `IO_16_8_1` |
| `0x02` | `IO_16x8_matrix` |
| `0x03` | `Out_8x10` |
| `0x04` | `Opto_16` |

The names are not decoration: they are the PlatformIO environment names, the CI
artefact names and the firmware filenames, all from one definition
(`BoardTypeName()`), so the three cannot drift apart.

The **build id** is the first 32 bits of the commit the firmware was built
from, or 0 when it was not recorded — a local build, say. The firmware version
is maintained by hand and does not move between releases, so it cannot
distinguish two dev snapshots; the build id is what can. Zero on either side
means "unknown" and never counts as a difference, so a hand-built board is not
reflashed on every start.

### Update sequence

    host                                board
     |-- UpdateBegin (size, CRC-16) ----->|   erase/prepare staging
     |<------------- UpdateBeginAck ------|   status + bytes staged
     |-- UpdateChunk (offset, len, ...) ->|   append at offset
     |<------------- UpdateChunkAck ------|   status + offset accepted
     |            ... repeated ...        |
     |-- UpdateCommit ------------------->|   verify CRC, then install
     |<------------- UpdateResult --------|   status

Bodies, after the two-byte `command`/`boardId` prefix:

| Command | Body |
|---|---|
| `UpdateBegin` | image size (4), image CRC-16 (2) |
| `UpdateBeginAck`, `UpdateChunkAck`, `UpdateResult` | status (1), offset (4) |
| `UpdateChunk` | offset (4), length (2), then `length` bytes, max 256 |
| `UpdateCommit` | none |

Chunks carry exactly one UF2 block's payload, which is what an image is made
of, so neither side repacks.

Every step is acknowledged with an explicit status rather than being inferred
from silence, so a refusal is distinguishable from a board that is not
answering:

| Value | Status | Meaning |
|---|---|---|
| `0x00` | `kUpdateOk` | |
| `0x01` | `kUpdateBusy` | an update is already in progress |
| `0x02` | `kUpdateTooLarge` | image will not fit the staging area |
| `0x03` | `kUpdateBadOffset` | chunk out of order or out of range |
| `0x04` | `kUpdateCrcMismatch` | staged image does not match the announced CRC |
| `0x05` | `kUpdateNotStaged` | commit without a complete transfer |
| `0x06` | `kUpdateWriteFailed` | flash refused the write |
| `0x07` | `kUpdateUnsupported` | this board cannot self-update |

### What the board does with the image

A board that accepts an `UpdateBegin` first turns its outputs off, by
dispatching the same restart it would on `kFrameRestart`. It is about to spend
some seconds not running the game and may reboot at the end of it, and the host
has no way to know what it was driving; a coil left energised through that
would burn.

Nothing irreversible until commit. Chunks are staged to a file in LittleFS and
the CRC is verified by reading that file back; only then is it handed to the
framework's OTA stage, which is what actually writes the application area on
the next boot. A power cut before commit loses the staged file and nothing
else; a power cut during the OTA write is what that bootloader is built to
survive.

This is deliberately a convenience path. Flashing over USB is always available
and is the fallback whenever the bus route is unavailable or refuses.

### Host behaviour

`ppuc-pinmame` queries every board's version at startup and prints what it
finds. Updating is opt-in:

| Option | Effect |
|---|---|
| `--firmware-path` | directory of `.uf2` images to compare against |
| `--allow-firmware-update` | update a board whose version is older than the image |
| `--allow-dev-firmware-update` | additionally, update when the version matches but the build id differs |

The second flag is separate because *different* is not *newer*. Without it, a
dev snapshot pointed at a machine running a release would quietly replace it.

A ppuc build ships the images it was built against in a `firmware/` directory
next to `plugins/`, so `--firmware-path firmware` on an unpacked release is the
normal case and the versions cannot disagree with the host binary.

Images are named `<board type>-<version>.uf2`, or
`<board type>-<version>+<build id>.uf2` for a snapshot. A name that cannot be
parsed in full is ignored rather than half-understood — a misread board type
would flash the wrong hardware.

A board that does not answer a version query is **left alone**: no type, no
version, no update. Those can still be flashed over USB, which is a far better
outcome than guessing at what an unresponsive board is.

---

## 8. Session lifecycle

### Epoch

`epoch` is a session generation number written into every frame by the host.
Boards record the epoch they last accepted and report it back as `epochSeen`.

A board that sees a new epoch treats the session as restarted: it discards
session state and expects a fresh `SetupFrame`. This allows recovery **without**
rebooting boards or dropping coil power, which matters because ordinary
transport desync must not depower a live playfield.

The host bumps the epoch after
`RS485_COMM_SWITCH_REPLY_MISS_THRESHOLD` (3) consecutive missed switch-reply
chains.

### Startup

1. **`RestartFrame`** — clear board-local configuration and runtime state,
   outputs off, board stays alive.
2. **`SetupFrame`** — announce `coilBits`, `lampBits`, `switchBits`. Idempotent
   and session-forming: it establishes the frame sizes everything else depends
   on.
3. **`MappingFrame`** ×N — map device index to global device number, per domain.
4. **`ConfigFrame`** ×N — per-device settings, each acknowledged with a
   `ConfigAckFrame`. Retried up to `RS485_COMM_CONFIG_ACK_RETRIES` (3) times,
   with a `RS485_COMM_CONFIG_ACK_TIMEOUT_US` (50 ms) window each.
5. Runtime begins.

---

## 9. Runtime loop

### Outputs, host → boards

The host broadcasts `OutputStateFrame` with the full coil, lamp and GI state,
every `kDefaultOutputFrameIntervalMs` (4 ms) by default. State is **snapshot
driven**: each frame is authoritative, so a lost frame is corrected by the next
one rather than needing retransmission.

### Switches, boards → host

The host starts a chain and each board answers in turn, naming its successor in
`nextBoard`. A board replies with either:

- **`kFrameSwitchState`** — status prefix plus the full switch bitmap, or
- **`kFrameSwitchNoChange`** — status prefix only, 11 bytes, when nothing changed.

The no-change variant exists because it is the overwhelmingly common case, and
sending 4 payload bytes instead of the full bitmap is most of the bus budget.

`kFrameSwitchRefresh` asks boards to report full state regardless of change, and
is issued after `kDefaultSwitchRefreshIdleMs` (15 s) without non-button switch
activity.

### Boards that only carry slow switches

A cabinet's flipper buttons sit on the boards that drive the flipper coils,
where latency is the entire point. The start button, coin door and tilt do not
care, and polling their board on every cycle costs every other board a reply's
worth of wire time — about 1 ms for the 11-byte no-change frame.

A board marked `slowSwitches: true` in the game YAML is therefore polled on
every eighth chain rather than every chain
(`RS485_COMM_SLOW_SWITCH_POLL_DIVIDER`). At the 4 ms output cadence that is
roughly every 32 ms, and every cycle in between is shorter for everyone else.

The mechanism is entirely host-side; boards are unaware of it. The chain is a
linked list configured onto the boards, so the host can **enter it late** — the
boards ahead of the entry point are simply not addressed and stay silent — but
cannot skip a board in the middle without reconfiguring its successor. Slow
boards are therefore sorted to the front of the chain when it is built,
regardless of their order in the YAML, so that skipping them is a matter of
choosing an entry point. Two rules keep it safe: a `kFrameSwitchRefresh` always
enters at the front, and a chain consisting only of slow boards is polled every
cycle, since there is nothing waiting behind it.

**Skipping does not lose transitions.** A board pushes a full switch snapshot
onto a 32-deep ring on every change, and a reply drains one entry, so a press
between two polls is delivered late rather than dropped. What a lower poll rate
does cost is drain time: a board with *n* queued transitions needs *n* polls to
report them all, so at the default divider that is ~32 ms per transition. That
is the reason the flag belongs only on boards whose switches are genuinely
slow. Ring overflow is reported as `kStatusSwitchOverflow`.

---

## 10. Configuration topics

`ConfigPayload.topic` selects what a `ConfigFrame` sets. Topics are defined in
`io-boards/src/EventDispatcher/Event.h`. Values are mostly ASCII letters, which
makes traces easier to read.

**Note:** several topics deliberately share a value because they apply to
different device types and can never appear in the same context — for example
`CONFIG_TOPIC_MIN_PULSE_TIME`, `CONFIG_TOPIC_FROM`, `CONFIG_TOPIC_MIN_INTENSITY`
and `CONFIG_TOPIC_DEBOUNCE_TIME` are all `77` ("M"). The topic alone does not
identify the field; it is interpreted relative to the section being configured.

Section topics: `PLATFORM` 102, `LED_STRING` 103, `LED_SEGMENT` 104,
`LED_EFFECT` 105, `PWM_EFFECT` 106, `LAMPS` 108, `MECHS` 109, `PWM` 112,
`COIN_DOOR_CLOSED_SWITCH` 113, `GAME_ON_SOLENOID` 114, `SWITCHES` 115,
`TRIGGER` 116, `SWITCH_MATRIX` 120, `SWITCH_CHAIN` 121.

Field topics include `NUMBER` 78, `PORT` 80, `POWER` 87, `TYPE` 89,
`MIN_PULSE_TIME` 77, `MAX_PULSE_TIME` 84, `HOLD_POWER` 72,
`HOLD_POWER_ACTIVATION_TIME` 65, `FAST_SWITCH` 70, `ACTIVE_LOW` 86,
`NEXT_BOARD` 88, `SWITCH_REPLY_DELAY_US` 93, and the LED-specific
`BRIGHTNESS` 66, `COLOR` 67, `AFTER_GLOW` 71, `LED_NUMBER` 76,
`AMOUNT_LEDS` 79, `LIGHT_UP` 85. `CONFIG_TOPIC_NULL` is 99.

---

## 11. Timing

### Board side

| Constant | Value | Meaning |
|---|---|---|
| `RS485_MODE_SWITCH_DELAY` | 50 µs | Between asserting DE and the first bit, and around releasing. |
| `switchReplyDelayUs` | **0** by default | Wait before asserting DE. Set via `--switch-reply-delay-us` or `SwitchReplyDelayUs` in the INI — **never** in the game YAML. |
| post-TX settle | `switchReplyDelayUs / 4`, capped 2000 µs | Stalls the board after it has already gone high-Z. |
| frame read timeout | frame wire time + 500 µs | How long `readBytes()` waits for a frame it has started reading. |

DE is released when the UART reports the last bit has left the shift register
(`uart0` BUSY clear), so the line is handed over as soon as the frame is
actually gone. The wait is bounded by a timeout derived from the estimated wire
time, so a stalled UART releases the bus rather than holding it.

The frame read timeout is the board's worst-case **blind time**: `readBytes()`
is a busy wait in the middle of the main loop, so while it spins nothing else
runs — not `PwmDevices::update()`, which enforces `maxPulseTime`, and not the
switch event dispatch. A sender transmits a frame continuously, so waiting
materially longer than its wire time cannot recover a frame already lost; it
only blocks the board. Worst case for one frame, with both the header and
payload reads timing out, is about 3.2 ms.

That figure is what sizes the host's switch reply window below — see the note
there.

### Host side

| Constant | Value | Meaning |
|---|---|---|
| `SwitchReplyWindowUs()` | 40 ms + `switchReplyDelayUs` × boards | Total window for a whole switch chain. |
| `RS485_COMM_SERIAL_READ_TIMEOUT` | 5 ms | Per-read serial timeout. |
| `RS485_COMM_SERIAL_WRITE_TIMEOUT` | 20 ms | Per-write. |
| `RS485_COMM_SWITCH_REPLY_MISS_THRESHOLD` | 3 | Consecutive misses before an epoch bump. |
| `RS485_COMM_CONFIG_ACK_TIMEOUT_US` | 50 ms | Per config-ack attempt. |
| `RS485_COMM_CONFIG_ACK_RETRIES` | 3 | Config frame attempts. |
| `RS485_COMM_SWITCH_POLL_STARTUP_HOLD_MS` | 250 | Startup hold. |
| `kDefaultOutputFrameIntervalMs` | 4 ms | Output broadcast interval. |
| `kDefaultSwitchRefreshIdleMs` | 15 s | Idle before a forced full refresh. |

Note that `switchReplyDelayUs` adds `delay × boards` to the board-side chain
**and** the same amount to the host's window, so with respect to that window the
two roughly cancel.

**Where the 40 ms comes from.** It is not arbitrary, though it reads that way.
A board occupies the chain for its reply on the wire, plus its blind time, plus
turnaround — and the chain is serial, so the window has to cover every board in
it:

| | per board | four-board chain |
|---|---|---|
| reply on the wire (64 switches) | 1.65 ms | |
| board blind time (frame read timeout) | ~1.1 ms | |
| turnaround | ~0.2 ms | |
| **total** | **~2.98 ms** | **~11.9 ms** |

The window therefore carries roughly 3.4× headroom over a four-board chain. It
is deliberately not tuned down to the calculation: the figures above are wire
time and known timeouts, and do not account for the board's own main-loop
latency, which is not currently measured. `PPUCBusHealth::switchReplyMisses`
is what shows whether the margin is being used.

`PPUC::GetBusHealth()` reports how often these recovery paths actually fire.

---

## 12. Missing features and future work

Recorded so the gaps are known rather than rediscovered.

### Sequence validation is not implemented

`sequence` is written into every frame and parsed on receipt, but **never
checked** for advance, loss or duplication. `kStatusSequenceGap` exists as a
status flag with nothing to set it on the host side.

Consequence: a lost or duplicated frame is only caught by the CRC and by
higher-level effects, not by the sequence number that exists to catch it. For
snapshot-driven output frames that is tolerable by design; for anything
edge-triggered it is not.

### Heartbeat and Error frames are reserved but unused

`kFrameHeartbeat` (0x03) and `kFrameError` (0x04) are defined, and both parsers
accept and skip them, but neither side ever sends one. There is therefore **no
board-to-host error reporting channel** — a board that rejects a config, drops
switch events or resynchronises its parser can only say so in the status flags
of its next switch reply, and only if it is asked for one.

### Pre-TX and post-TX delays are not independently configurable

The post-TX settle is derived as `switchReplyDelayUs / 4`, so shortening the
pre-TX delay necessarily shortens the settle. A short pre-TX delay with a
generous settle cannot currently be expressed, which makes a delay sweep harder
to interpret. Splitting them into two config topics is proposed in
`STABILIZATION_PLAN.md` §2.4.

### No on-wire version negotiation

The header has no protocol version field. Host and boards are matched by the
SHA pin chain at build time, not by handshake, so a mismatched pair fails in
whatever way the first incompatible frame happens to fail. `ppucVersion` in the
game YAML versions the *configuration*, not the wire format.

### Bus is at 46 % of the transceiver's ceiling

115200 baud against the ADM3483's 250 kbps limit. There is real headroom, and
the no-change reply already keeps the common case small. Raising it is a
separate question from stability and should not be attempted until the
outstanding turnaround measurement is done — see `BUS_MEASUREMENT.md`.

### Asymmetric device limits

`kMaxCoilBits` is 64 while lamps and switches allow 256. Coils are the scarcest
resource on a board, so this is defensible, but it is worth knowing before
designing a game that wants more than 64 coil numbers across the whole machine.

### Output frames are unacknowledged

By design — state is snapshot driven and the next frame corrects any loss. Noted
because it is a reasonable thing to look for and not find: there is no
`OutputAck`, and adding one would trade the property that makes recovery cheap.

