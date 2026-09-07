#!/usr/bin/env bash
#
# End-to-end check that solenoid edges still reach the host.
#
# PluginEngine polls PinMAME's solenoid states on a dedicated 1 kHz thread and
# dispatches edges outside the quiesce gate. Every unit test around that path
# tests a piece of it; nothing tests that a running ROM's coil actually arrives.
# A wedged poll thread, a gate that never reopens, and a machine simply sitting
# in attract all look identical -- silence.
#
# So this drives a real ROM to the one moment it must fire: ball in the outhole,
# coin, start button, kick-out coil. Time Warp is used because its switch and
# coil numbering is pinned in ppuc_games/tmwrp/io-boards.yaml.
#
# Needs a ROM, so it is not part of ppuc_tests. Exits 77 (skip) when the game
# assets are not checked out next to this repo.
#
# Overrides: PPUC_BIN, PPUC_GAME.

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
BIN="${PPUC_BIN:-${ROOT}/ppuc/ppuc-pinmame}"
GAME="${PPUC_GAME:-${ROOT}/../ppuc_games/tmwrp}"
RULES="${ROOT}/tests/fixtures/tmwrp_coldstart.lua"
RUN_MS=15000

if [ ! -x "${BIN}" ]; then
   echo "SKIP: no ppuc-pinmame at ${BIN}; run ./build.sh first"
   exit 77
fi
if [ ! -f "${GAME}/pinmame/roms/tmwrp_l2.zip" ]; then
   echo "SKIP: no tmwrp_l2 ROM under ${GAME}"
   exit 77
fi

LOG="$(mktemp -t ppuc_tmwrp_coldstart)"
trap 'rm -f "${LOG}"' EXIT

# -n drops the RS485 bus so no boards are needed; display and sound are off so
# this runs headless on a build machine.
"${BIN}" \
   --game "${GAME}" \
   -n --no-display --no-sound \
   --plugin-dir "$(dirname "${BIN}")/plugins" \
   --rules "${RULES}" \
   --exit-after-ms "${RUN_MS}" >"${LOG}" 2>&1
status=$?

verdict="$(grep -m1 '^TEST-RESULT: ' "${LOG}")"

if [ -z "${verdict}" ]; then
   echo "FAIL: the fixture never reported a verdict (exit ${status})."
   echo "      Either the run died early or --rules was not honoured."
   tail -n 30 "${LOG}"
   exit 1
fi

echo "${verdict}"
case "${verdict}" in
   "TEST-RESULT: PASS"*) exit 0 ;;
   *) tail -n 30 "${LOG}"; exit 1 ;;
esac
