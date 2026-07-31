# V2 Resync Proposal

> **Status: largely implemented.** This is design rationale and historical
> record, not a plan for future work. Verified against the tree on 2026-07-31:
>
> **Shipped as proposed**
> - 5-byte `FrameHeader` with `epoch` (`kHeaderBytes = 5`)
> - `SwitchStatusPayload` on both switch reply variants, with all six flags
>   under the proposed names: `kStatusInSync`, `kStatusNeedsSetup`,
>   `kStatusMappingIncomplete`, `kStatusSequenceGap`, `kStatusParserResynced`,
>   `kStatusSwitchOverflow`
> - host-side `m_epoch`, epoch written into all frames, epoch-based session
>   resync after `RS485_COMM_SWITCH_REPLY_MISS_THRESHOLD` consecutive misses
> - `ResetFrame` reserved for hard reboot only
>
> **Not implemented**
> - Sequence validation. `sequence` is transmitted and parsed but never checked
>   for advance, loss or duplication, so `lastAckedSequencePerBoard` and step 3
>   of the "Normal runtime loop" below do not exist. Still open work.
>
> **Superseded**
> - Every reference to UART DMA RX is obsolete. DMA was not merely "unstable" —
>   it has since been **removed from the firmware entirely**, and the blocking
>   framed parser is the only RX path. Ignore the DMA items in "Current Problem
>   Summary" and "Implementation Order".
> - The proposal predates `kFrameRestart (0x0B)`. `RestartFrame` became the
>   actual normal-path answer to "recover without rebooting": it clears
>   board-local config and runtime state and turns outputs off while keeping the
>   RP2040 alive on UART. It is now the standard startup and shutdown path, with
>   epoch resync handling mid-session recovery. See `ppuc/docs/STACK.md` §5.

## Goal

Recover RS485/session sync without:

- rebooting boards
- dropping coil power during normal gameplay
- losing fast switch changes

Hard reset remains available for true safety faults, but ordinary transport
desync must not depower outputs.

## Current Problem Summary

*Historical — these were the observed failure modes when this document was
written. See the status block above for what has since changed.*

Observed failure modes:

- startup reset is not fully robust
- runtime communication can stall after minutes of normal operation
- once stalled, restarting `ppuc` may not recover boards without a power cycle
- UART DMA RX cutover is not yet stable on hardware

The current V2 protocol has no explicit session resync mechanism other than
hard reset.

## Design Principles

1. Output state must remain authoritative and recoverable from the next valid
   host snapshot.
2. Switch capture must continue locally even while transport/session state is
   being repaired.
3. Parser resync, session resync, and hard reset must be separate concepts.
4. Runtime recovery must be possible without clearing applied outputs.

## Protocol Changes

### 1. Extend the V2 header with `epoch`

Current header:

```c++
struct FrameHeader {
  uint8_t sync;
  uint8_t typeAndFlags;
  uint8_t nextBoard;
  uint8_t sequence;
};
```

Proposed header:

```c++
struct FrameHeader {
  uint8_t sync;
  uint8_t typeAndFlags;
  uint8_t nextBoard;
  uint8_t sequence;
  uint8_t epoch;
};
```

Notes:

- `epoch` is owned by the host.
- Host increments `epoch` whenever it wants a session resync.
- All runtime frames must carry the current `epoch`.
- `kHeaderBytes` becomes `5`.

### 2. Add switch/status acknowledgment payload

Current board-to-host switch frames return only switch bitmap or no-change.

Add a small status prefix to both switch reply variants:

```c++
struct SwitchStatusPayload {
  uint8_t epochSeen;
  uint8_t lastHostSequenceSeen;
  uint8_t flags;
  uint8_t reserved;
};
```

Proposed flags:

- bit 0: `kStatusInSync`
- bit 1: `kStatusNeedsSetup`
- bit 2: `kStatusMappingIncomplete`
- bit 3: `kStatusSequenceGap`
- bit 4: `kStatusParserResynced`
- bit 5: `kStatusSwitchOverflow`

Then:

```c++
struct SwitchPayload {
  SwitchStatusPayload status;
  uint8_t switches[kMaxSwitchBytes];
};
```

`SwitchNoChangeFrame` should still carry the same `SwitchStatusPayload` even
without switch bitmap bytes.

### 3. Make `SetupFrame` idempotent and session-forming

`SetupFrame` becomes:

- initial session bootstrap
- session resync entry point
- not a hard reset

Receiving a valid `SetupFrame` for a new epoch must:

- accept the new session epoch
- clear parser/session bookkeeping
- clear mapping-valid flags
- reset expected sequence tracking
- reset token/switch-chain state
- preserve currently applied outputs until replaced by a valid output frame
- preserve local switch capture state

### 4. Keep `ResetFrame` as hard reset only

`ResetFrame` should remain the "hard fault / operator requested reboot" path.

It must not be used for ordinary desync recovery.

## Board State Machine

Maintain these state groups separately:

### Transport/parser state

- `currentEpoch`
- `lastHostSequenceSeen`
- `parserState`
- `sequenceGapDetected`
- `parserResynced`

### Session/config state

- `runtimeConfigValid`
- `mappingValid`
- `mappingGenerationComplete`
- `nextSwitchBoard`

### Runtime/output state

- `appliedOutputCoils`
- `appliedOutputLamps`
- `appliedOutputGi`

### Switch capture state

- `switchCurrentBitmap`
- `switchDirtyBitmap`
- optional `switchOverflow` / edge overflow flag

### Recovery rules

#### Parser resync

Triggered by:

- sync loss
- CRC failure
- illegal/truncated frame

Action:

- discard partial frame
- scan for next sync byte
- set `kStatusParserResynced`
- do not clear outputs
- do not clear switch state

#### Session resync

Triggered by:

- new `epoch`
- host explicitly restarts session via `SetupFrame`

Action:

- adopt new epoch
- clear sequence tracking and token-chain bookkeeping
- mark setup/mapping as pending until replayed
- preserve outputs until next valid output snapshot
- preserve switch capture

#### Hard reset

Triggered by:

- `ResetFrame`
- unrecoverable firmware fault
- operator/safety action

Action:

- current reboot behavior
- outputs are allowed to depower

## Host State Machine

Maintain:

- `currentEpoch`
- `currentSequence`
- `lastAckedSequencePerBoard`
- `boardStatusFlags`

### Normal runtime loop

1. Send `OutputStateFrame(epoch, seq, nextBoard)`
2. Receive `SwitchStateFrame` or `SwitchNoChangeFrame`
3. Check:
   - `epochSeen == currentEpoch`
   - `lastHostSequenceSeen` advances
   - status flags do not indicate setup/mapping loss

### Host recovery ladder

#### Stage 1: parser tolerance

If one response is malformed or missing:

- retry normal output loop for a small number of cycles
- do not reset outputs

#### Stage 2: session resync

If a board stays stale:

1. `currentEpoch++`
2. resend `SetupFrame`
3. resend mapping frames
4. resend config frames
5. resume normal output snapshots

This must not use `ResetFrame`.

#### Stage 3: hard reset

Only if:

- board remains unreachable after session resync
- operator explicitly requests it
- safety state requires reboot

## Output-State Rule

Output frames remain full snapshots.

This is what makes non-disruptive resync possible:

- the next valid output frame can fully reassert coil/lamp/GI state
- boards do not need to clear outputs to recover protocol/session state

## Switch-State Rule

Switch capture must be local-first and continuous.

Recommended behavior:

- board continues capturing switches even while out of session sync
- board reports full bitmap plus status on token/poll
- board keeps a dirty/overflow indication so host can detect that activity
  happened even if timing was imperfect

Fast local reactions such as fast-flip must stay board-local and must not
depend on host round trips.

## Concrete Header/File Changes

### `io-boards/src/PPUCProtocolV2.h`

Make these structural changes:

- `kHeaderBytes = 5`
- extend `FrameHeader` with `uint8_t epoch`
- add `SwitchStatusPayload`
- prepend switch status to switch reply payload calculations
- add status flag constants

Likely helper additions:

- `inline bool EpochChanged(uint8_t current, uint8_t incoming)`
- `inline bool SequenceAdvanced(uint8_t prev, uint8_t next)`

### `libppuc/src/RS485Comm.cpp`

Add host-side support for:

- `m_epoch`
- writing epoch into all runtime/config/setup/reset frames
- reading `epochSeen`, `lastHostSequenceSeen`, and flags from switch replies
- session resync ladder

### `io-boards/src/EventDispatcher/EventDispatcher.cpp`

Add board-side support for:

- `currentEpoch`
- sequence tracking
- parser-only resync
- session resync on new `SetupFrame`
- switch status population in reply frames
- preserving outputs across session resync

## Implementation Order

*Historical. Steps 1–6 were completed; step 7 was overtaken by events — DMA RX
was removed from the firmware rather than revisited.*

1. Update `PPUCProtocolV2.h` with header/status changes.
2. Update board fallback RX path first, not DMA.
3. Update host send/receive path for epoch and switch status.
4. Make `SetupFrame` idempotent and session-forming.
5. Add host session resync ladder.
6. Validate long-running runtime without DMA.
7. ~~Revisit DMA RX only after the session-resync-capable non-DMA path is
   stable.~~ DMA RX was removed instead.

## Non-Goals For First Pass

Do not add these in the first implementation:

- per-frame ACKs for every config/output frame
- output delta compression
- switch edge streaming instead of bitmap snapshots
- automatic hard reset on first sequence mismatch

The first pass should stay simple:

- full output snapshots
- full switch bitmaps plus status
- epoch-based session resync
- hard reset reserved for explicit/safety cases
