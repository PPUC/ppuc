# AGENTS.md

## Scope

This repository contains the `ppuc` applications, especially:

- `ppuc-pinmame`: game runtime and machine integration
- `ppuc-backbox`: backbox/display runtime

The command-line app sits above `../libppuc` and indirectly above `../io-boards`.

## Working Notes

- Treat `src/ppuc.cpp` as the main runtime entry point for machine behavior.
- Test-mode options (`--switch-test`, `--lamp-test`, `--gi-test`, `--flasher-test`, `--coil-test`) depend on `libppuc` behavior. If a regression appears there, inspect `../libppuc/src/PPUC.cpp` and `../libppuc/src/RS485Comm.cpp` first.
- For non-WPC games, GI behavior may be forced from `libppuc` rather than driven by PinMAME GI updates.
- `ppuc-pinmame` owns host-side ball search. It is disabled by default and is enabled with `--ball-search` or `Runtime.BallSearch=true`; only coils marked `ballSearch: true` in YAML are pulsed.
- Switch refresh is always active by default through `--switch-refresh-idle-ms` / `Runtime.SwitchRefreshIdleMs` and uses `button: true` switch metadata to ignore cabinet/flipper button activity for the idle decision.
- Runtime output/switch-poll cadence is configurable with `--output-frame-interval-ms` / `Runtime.OutputFrameIntervalMs`; the default remains 4 ms.

## Confirmed Cross-Layer Finding

- Real-machine testing showed that `libppuc` switch-chain timing directly affects visible gameplay/output quality in `ppuc`, not only switch diagnostics.
- After relaxing host-side switch-reply timing and making session resync less aggressive, lamp attract-mode animation became visibly correct again.
- For future games with more IO boards, treat switch-chain timing as a transport tuning area that can affect normal runtime presentation.

Practical implication:

- If lamps, GI, or switch tests look "mostly alive but wrong", do not assume the issue lives only in `ppuc`.
- Check whether `libppuc` is thrashing V2 session resync or timing out on switch replies.

## Virtual Board Notes

- First-slice virtual board support now lives in `libppuc`.
- Presence for configured boards is now based on an explicit startup handshake in `libppuc`, not on later switch-reply traffic.
- `ppuc` should treat `--close-coin-door` as a virtual-board override only.
- `ppuc` also supports `--skip-boards <csv>` for fast bench testing.
- Skipped boards are treated as virtual immediately, receive no config frames, and are omitted from direct lamp/coil/switch test walks.
- If the configured coin door switch belongs to a present physical board, `ppuc` should ignore the override and report that ownership remains with hardware.
- This supports one game config for cabinet and bench use without turning host-side switch injection into a global override mechanism.
