#!/usr/bin/env bash
#
# A ROM PinMAME does not know must end ppuc-pinmame with status 1, cleanly.
#
# PinMAME only says it does not know a game after PluginEngine has subscribed
# to the plugin bus and created the controller. Start() used to return with
# both still in place, and they were destroyed after the bus had unloaded the
# plugin they point into: PinMAME printed that its controller was never
# destroyed, and the process died with SIGSEGV. On a machine that is a crash
# loop with nothing in the log saying why.
#
# Needs no ROM, only the packaged binary and a game folder with a config.
#
# Overrides: PPUC_BIN, PPUC_GAME.

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
BIN="${PPUC_BIN:-${ROOT}/ppuc/ppuc-pinmame}"
GAME="${PPUC_GAME:-${ROOT}/../ppuc_games/tmwrp}"

if [ ! -x "${BIN}" ]; then
   echo "SKIP: no ppuc-pinmame at ${BIN}; run ./build.sh first"
   exit 77
fi
if [ ! -f "${GAME}/io-boards.yaml" ]; then
   echo "SKIP: no game folder at ${GAME}"
   exit 77
fi

LOG="$(mktemp -t ppuc_unknown_rom)"
# A game folder with nothing but the wiring: no translite, so no window, and no
# state/ written into ppuc_games.
#
# Known flake, not what this tests: on macOS a run now and then dies while the
# plugin bus loads the plugins, before PinMAME is asked for the ROM (status 132
# or 139, last line "Plugin directory: ..."). It happens on commits before this
# test existed too.
FOLDER="$(mktemp -d -t ppuc_unknown_rom_game)"
cp "${GAME}/io-boards.yaml" "${FOLDER}/"
trap 'rm -f "${LOG}"; rm -rf "${FOLDER}"' EXIT

"${BIN}" \
   --game "${FOLDER}" \
   --rom ppuc_no_such_rom \
   -n --no-display --no-sound \
   --plugin-dir "$(dirname "${BIN}")/plugins" \
   --exit-after-ms 5000 >"${LOG}" 2>&1
status=$?

if [ "${status}" -ne 1 ]; then
   echo "FAIL: expected exit status 1 for an unknown ROM, got ${status}."
   tail -n 20 "${LOG}"
   exit 1
fi
if grep -q "was not destroyed" "${LOG}"; then
   echo "FAIL: the PinMAME controller outlived the engine."
   grep "was not destroyed" "${LOG}"
   exit 1
fi
if ! grep -q "ppuc_no_such_rom" "${LOG}"; then
   echo "FAIL: the unknown ROM was not reported."
   tail -n 20 "${LOG}"
   exit 1
fi
echo "TEST-RESULT: PASS unknown ROM exits 1 cleanly"
exit 0
